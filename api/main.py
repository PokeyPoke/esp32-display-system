import os
import sqlite3
import secrets
from datetime import datetime, timedelta, timezone
from fastapi import FastAPI, HTTPException
from pydantic import BaseModel

# Minimal config
DB_PATH = os.getenv("DB_PATH", "/tmp/app.db")
app = FastAPI(title="ESP32 Display API Minimal", version="0.1.0")

def now_utc() -> datetime:
    return datetime.now(timezone.utc)

def init_db():
    conn = sqlite3.connect(DB_PATH, check_same_thread=False)
    conn.row_factory = sqlite3.Row
    cur = conn.cursor()
    cur.execute("""
        CREATE TABLE IF NOT EXISTS device (
            id TEXT PRIMARY KEY,
            hardware_uid TEXT UNIQUE,
            device_token TEXT,
            created_at TEXT
        )
    """)
    cur.execute("""
        CREATE TABLE IF NOT EXISTS pairing (
            pair_code TEXT PRIMARY KEY,
            device_id TEXT,
            expires_at TEXT,
            claimed_at TEXT
        )
    """)
    conn.commit()
    conn.close()

init_db()

class PairStartIn(BaseModel):
    hardware_uid: str

class PairStartOut(BaseModel):
    pair_code: str
    device_token: str
    device_id: str
    expires_in: int

def generate_id() -> str:
    return f"dev_{secrets.token_urlsafe(9)}"

def generate_code() -> str:
    return f"{secrets.randbelow(1000000):06d}"

def generate_token() -> str:
    return secrets.token_urlsafe(24)

@app.post("/pair/start", response_model=PairStartOut)
def pair_start(inp: PairStartIn):
    print(f"Pairing request for: {inp.hardware_uid}")
    
    conn = sqlite3.connect(DB_PATH)
    conn.row_factory = sqlite3.Row
    cur = conn.cursor()
    
    # Get or create device
    row = cur.execute("SELECT id FROM device WHERE hardware_uid=?", (inp.hardware_uid,)).fetchone()
    if row:
        device_id = row["id"]
    else:
        device_id = generate_id()
        cur.execute(
            "INSERT INTO device (id, hardware_uid, device_token, created_at) VALUES (?, ?, ?, ?)",
            (device_id, inp.hardware_uid, "", now_utc().isoformat())
        )
    
    # Generate tokens
    device_token = generate_token()
    pair_code = generate_code()
    
    cur.execute("UPDATE device SET device_token=? WHERE id=?", (device_token, device_id))
    
    expires_at = (now_utc() + timedelta(seconds=300)).isoformat()
    cur.execute(
        "INSERT OR REPLACE INTO pairing (pair_code, device_id, expires_at, claimed_at) VALUES (?, ?, ?, ?)",
        (pair_code, device_id, expires_at, None)
    )
    
    conn.commit()
    conn.close()
    
    print(f"Generated pair code: {pair_code}")
    return PairStartOut(
        pair_code=pair_code,
        device_token=device_token,
        device_id=device_id,
        expires_in=300
    )

@app.get("/healthz")
def healthz():
    return {"ok": True, "version": "minimal-v2", "timestamp": str(now_utc())}

@app.get("/")
def root():
    return {"message": "ESP32 Display API - Minimal Version", "status": "working"}

if __name__ == "__main__":
    import uvicorn
    port = int(os.getenv("PORT", "8000"))
    uvicorn.run("main:app", host="0.0.0.0", port=port)