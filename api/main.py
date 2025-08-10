
import os
import sqlite3
import secrets
import time
import json
import logging
import contextlib
from datetime import datetime, timedelta, timezone
from typing import Optional, Dict, Any, List
from contextlib import contextmanager
from functools import lru_cache
from threading import Lock

import uvicorn
from fastapi import FastAPI, HTTPException, Request, Response, Depends, Cookie
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import JSONResponse, FileResponse
from pydantic import BaseModel, Field, validator
from slowapi import Limiter, _rate_limit_exceeded_handler
from slowapi.util import get_remote_address
from slowapi.errors import RateLimitExceeded
from slowapi.middleware import SlowAPIMiddleware
import httpx

# Configuration
APP_SECRET = os.getenv("APP_SECRET", "change-me-in-prod")
PAIR_TTL_SECONDS = int(os.getenv("PAIR_TTL_SECONDS", "300"))
SESSION_TTL_MINUTES = int(os.getenv("SESSION_TTL_MINUTES", "30"))
DEVICE_TOKEN_TTL_HOURS = int(os.getenv("DEVICE_TOKEN_TTL_HOURS", "8760"))  # 1 year
ALLOWED_ORIGINS = os.getenv("ALLOWED_ORIGINS", "*").split(",")
ENVIRONMENT = os.getenv("ENVIRONMENT", "development")

# Validate critical config
if ENVIRONMENT == "production" and APP_SECRET == "change-me-in-prod":
    raise ValueError("APP_SECRET must be set in production")

DB_PATH = os.getenv("DB_PATH", "app.db")
os.makedirs(os.path.dirname(DB_PATH) if os.path.dirname(DB_PATH) else ".", exist_ok=True)

# Logging setup
logging.basicConfig(
    level=logging.INFO if ENVIRONMENT == "production" else logging.DEBUG,
    format='%(asctime)s - %(name)s - %(levelname)s - %(message)s'
)
logger = logging.getLogger(__name__)

# Database connection pool
db_lock = Lock()
_db_connection = None

def now_utc() -> datetime:
    return datetime.now(timezone.utc)

@contextmanager
def get_db():
    """Thread-safe database connection context manager with connection pooling."""
    global _db_connection
    with db_lock:
        if _db_connection is None:
            _db_connection = sqlite3.connect(DB_PATH, check_same_thread=False)
            _db_connection.row_factory = sqlite3.Row
            _db_connection.execute("PRAGMA journal_mode=WAL")
            _db_connection.execute("PRAGMA synchronous=NORMAL")
            _db_connection.execute("PRAGMA cache_size=10000")
        conn = _db_connection
    try:
        yield conn
    except Exception:
        conn.rollback()
        raise
    else:
        conn.commit()

def init_db() -> None:
    """Initialize database with improved schema including token expiration."""
    with get_db() as conn:
        cur = conn.cursor()
        cur.executescript(
            """
            PRAGMA journal_mode=WAL;
            CREATE TABLE IF NOT EXISTS device (
                id TEXT PRIMARY KEY,
                hardware_uid TEXT UNIQUE,
                device_token TEXT,
                device_token_expires_at TEXT,
                created_at TEXT
            );
            CREATE TABLE IF NOT EXISTS pairing (
                pair_code TEXT PRIMARY KEY,
                device_id TEXT,
                expires_at TEXT,
                claimed_at TEXT
            );
            CREATE INDEX IF NOT EXISTS idx_pairing_expires ON pairing(expires_at);
            CREATE TABLE IF NOT EXISTS module (
                device_id TEXT PRIMARY KEY,
                type TEXT,
                params_json TEXT,
                updated_at TEXT
            );
            CREATE TABLE IF NOT EXISTS session (
                session_token TEXT PRIMARY KEY,
                device_id TEXT,
                expires_at TEXT
            );
            CREATE INDEX IF NOT EXISTS idx_session_expires ON session(expires_at);
            CREATE INDEX IF NOT EXISTS idx_device_token_expires ON device(device_token_expires_at);
            """
        )

init_db()

app = FastAPI(title="ESP32 Minimal Display API", version="0.1.0")

# CORS for local dev
app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_credentials=True,
    allow_methods=["*"],
    allow_headers=["*"],
)

# ---------- Models ----------
class PairStartIn(BaseModel):
    hardware_uid: str

class PairStartOut(BaseModel):
    pair_code: str
    device_token: str
    device_id: str
    expires_in: int

class PairClaimIn(BaseModel):
    pair_code: str

class PairClaimOut(BaseModel):
    device_id: str

class ModuleIn(BaseModel):
    type: str
    params: Dict[str, Any]

# ---------- Helpers ----------
def generate_id(prefix="dev") -> str:
    return f"{prefix}_{secrets.token_urlsafe(9)}"

def generate_code() -> str:
    # 6-digit numeric code
    return f"{secrets.randbelow(1000000):06d}"

def generate_token(nbytes=24) -> str:
    return secrets.token_urlsafe(nbytes)

def get_session_device_id(sess: Optional[str]) -> str:
    if not sess:
        raise HTTPException(status_code=401, detail="No session")
    conn = db()
    row = conn.execute("SELECT device_id, expires_at FROM session WHERE session_token=?", (sess,)).fetchone()
    if not row:
        raise HTTPException(status_code=401, detail="Invalid session")
    if datetime.fromisoformat(row["expires_at"]) < now_utc():
        raise HTTPException(status_code=401, detail="Session expired")
    return row["device_id"]

# Simple in-memory cache for external API calls
CACHE: Dict[str, Dict[str, Any]] = {}

def cache_get(key: str) -> Optional[Dict[str, Any]]:
    item = CACHE.get(key)
    if not item:
        return None
    if item["expires_at"] < time.time():
        return None
    return item["value"]

def cache_set(key: str, value: Dict[str, Any], ttl: int):
    CACHE[key] = {"value": value, "expires_at": time.time() + ttl}

# ---------- External data fetchers ----------
async def get_btc_price() -> Dict[str, Any]:
    key = "btc_price"
    cached = cache_get(key)
    if cached:
        return cached
    # CoinGecko simple price (no key)
    url = "https://api.coingecko.com/api/v3/simple/price"
    params = {"ids": "bitcoin", "vs_currencies": "usd", "include_24hr_change": "true"}
    async with httpx.AsyncClient(timeout=8) as client:
        r = await client.get(url, params=params)
        r.raise_for_status()
        data = r.json()
    price = float(data["bitcoin"]["usd"])
    chg = float(data["bitcoin"].get("usd_24h_change", 0.0))
    out = {"price_usd": price, "change_24h": chg}
    cache_set(key, out, ttl=12)  # 12s cache
    return out

async def get_weather(city: str) -> Dict[str, Any]:
    # For simplicity, expect "City,CC". We'll use Open-Meteo's geocoding to get lat/lon
    key = f"wx_{city}"
    cached = cache_get(key)
    if cached:
        return cached
    async with httpx.AsyncClient(timeout=8) as client:
        geo = await client.get("https://geocoding-api.open-meteo.com/v1/search", params={"name": city, "count": 1})
        geo.raise_for_status()
        g = geo.json()
        if not g.get("results"):
            raise HTTPException(400, f"City not found: {city}")
        lat = g["results"][0]["latitude"]
        lon = g["results"][0]["longitude"]
        wx = await client.get(
            "https://api.open-meteo.com/v1/forecast",
            params={"latitude": lat, "longitude": lon, "current_weather": True}
        )
        wx.raise_for_status()
        w = wx.json()
    cw = w.get("current_weather", {})
    out = {
        "temp_c": cw.get("temperature"),
        "windspeed_kph": cw.get("windspeed"),
        "condition_code": cw.get("weathercode"),
        "city": city
    }
    cache_set(key, out, ttl=300)  # 5 min cache
    return out

# ---------- Routes ----------
@app.post("/pair/start", response_model=PairStartOut)
def pair_start(inp: PairStartIn):
    conn = db()
    cur = conn.cursor()
    # Ensure device exists or create
    row = cur.execute("SELECT id FROM device WHERE hardware_uid=?", (inp.hardware_uid,)).fetchone()
    if row:
        device_id = row["id"]
    else:
        device_id = generate_id("dev")
        cur.execute(
            "INSERT INTO device (id, hardware_uid, device_token, created_at) VALUES (?, ?, ?, ?)",
            (device_id, inp.hardware_uid, "", now_utc().isoformat())
        )
    # New device token each pairing start
    device_token = generate_token(24)
    cur.execute("UPDATE device SET device_token=? WHERE id=?", (device_token, device_id))

    # Create a unique 6-digit code
    # Try a few times to avoid collision
    for _ in range(5):
        pair_code = generate_code()
        exists = cur.execute("SELECT 1 FROM pairing WHERE pair_code=?", (pair_code,)).fetchone()
        if not exists:
            break
    else:
        raise HTTPException(500, "Failed to allocate pair code")

    expires_at = (now_utc() + timedelta(seconds=PAIR_TTL_SECONDS)).isoformat()
    cur.execute(
        "INSERT OR REPLACE INTO pairing (pair_code, device_id, expires_at, claimed_at) VALUES (?, ?, ?, ?)",
        (pair_code, device_id, expires_at, None)
    )
    conn.commit()
    conn.close()
    return PairStartOut(pair_code=pair_code, device_token=device_token, device_id=device_id, expires_in=PAIR_TTL_SECONDS)

@app.post("/pair/claim", response_model=PairClaimOut)
def pair_claim(inp: PairClaimIn, response: Response = None):
    conn = db()
    cur = conn.cursor()
    row = cur.execute("SELECT device_id, expires_at, claimed_at FROM pairing WHERE pair_code=?", (inp.pair_code,)).fetchone()
    if not row:
        raise HTTPException(400, "Invalid code")
    if row["claimed_at"] is not None:
        raise HTTPException(400, "Code already claimed")
    if datetime.fromisoformat(row["expires_at"]) < now_utc():
        raise HTTPException(400, "Code expired")

    device_id = row["device_id"]
    # Create a web session
    sess = generate_token(24)
    expires_at = (now_utc() + timedelta(minutes=SESSION_TTL_MINUTES)).isoformat()
    cur.execute("INSERT INTO session (session_token, device_id, expires_at) VALUES (?, ?, ?)", (sess, device_id, expires_at))
    cur.execute("UPDATE pairing SET claimed_at=? WHERE pair_code=?", (now_utc().isoformat(), inp.pair_code))
    conn.commit()
    conn.close()

    # Set cookie
    if response is not None:
        response.set_cookie("sess", sess, max_age=SESSION_TTL_MINUTES*60, httponly=True, samesite="Lax")
    return PairClaimOut(device_id=device_id)

@app.post("/device/{device_id}/module")
def set_module(device_id: str, mod: ModuleIn, sess: Optional[str] = Cookie(default=None)):
    # Auth via session cookie
    sess_device_id = get_session_device_id(sess)
    if sess_device_id != device_id:
        raise HTTPException(403, "Session not authorized for this device")
    if mod.type not in {"text", "btc_price", "weather"}:
        raise HTTPException(400, "Unsupported module type")

    conn = db()
    cur = conn.cursor()
    cur.execute(
        "INSERT INTO module (device_id, type, params_json, updated_at) VALUES (?, ?, ?, ?) "
        "ON CONFLICT(device_id) DO UPDATE SET type=excluded.type, params_json=excluded.params_json, updated_at=excluded.updated_at",
        (device_id, mod.type, json.dumps(mod.params), now_utc().isoformat())
    )
    conn.commit()
    conn.close()
    return {"ok": True}

@app.get("/device/config")
async def device_config(device_token: str):
    # Auth via device token
    conn = db()
    cur = conn.cursor()
    row = cur.execute("SELECT id FROM device WHERE device_token=?", (device_token,)).fetchone()
    if not row:
        raise HTTPException(401, "Invalid device token")
    device_id = row["id"]
    mrow = cur.execute("SELECT type, params_json FROM module WHERE device_id=?", (device_id,)).fetchone()
    conn.close()

    if not mrow:
        # Not configured yet
        return {
            "render": {
                "lines": ["Not configured", "Pick module in web UI"],
                "ttl": 15
            },
            "next_poll_sec": 10
        }

    mtype = mrow["type"]
    params = json.loads(mrow["params_json"] or "{}")
    lines = []
    ttl = 15
    next_poll = 10

    try:
        if mtype == "text":
            msg = str(params.get("message", "")).strip()
            if not msg:
                msg = "Hello from ESP32!"
            # split into max 16 chars per line (simple)
            width = int(params.get("max_chars", 16))
            lines = [msg[i:i+width] for i in range(0, len(msg), width)][:4]
            ttl = 60
            next_poll = 15

        elif mtype == "btc_price":
            data = await get_btc_price()
            price = data["price_usd"]
            chg = data["change_24h"]
            sign = "+" if chg >= 0 else ""
            lines = [f"BTC ${price:,.0f}", f"24h {sign}{chg:.2f}%"]
            ttl = 12
            next_poll = 10

        elif mtype == "weather":
            city = params.get("city", "Portland,US")
            data = await get_weather(city)
            temp = data.get("temp_c")
            wind = data.get("windspeed_kph")
            lines = [data.get("city", city), f"{temp}°C  Wind {wind}"]
            ttl = 300
            next_poll = 60

    except Exception as e:
        # Return error line, short TTL to retry
        lines = ["Err fetching data", str(e)[:18]]
        ttl = 5
        next_poll = 5

    return {"render": {"lines": lines, "ttl": ttl}, "next_poll_sec": next_poll}

@app.get("/healthz")
def healthz():
    return {"ok": True}

# Serve the minimal web UI if hosted together
@app.get("/")
def root():
    return FileResponse("web/index.html")

if __name__ == "__main__":
    port = int(os.getenv("PORT", "8000"))
    uvicorn.run("main:app", host="0.0.0.0", port=port, reload=False)
