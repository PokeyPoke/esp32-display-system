import secrets
from fastapi import FastAPI
from pydantic import BaseModel

app = FastAPI(title="ESP32 Test API")

class PairStartIn(BaseModel):
    hardware_uid: str

class PairStartOut(BaseModel):
    pair_code: str
    device_token: str
    device_id: str
    expires_in: int

@app.post("/pair/start", response_model=PairStartOut)
def pair_start(inp: PairStartIn):
    # No database - just generate and return
    return PairStartOut(
        pair_code=f"{secrets.randbelow(1000000):06d}",
        device_token=secrets.token_urlsafe(24),
        device_id=f"dev_{secrets.token_urlsafe(9)}",
        expires_in=300
    )

@app.get("/healthz")
def healthz():
    return {"ok": True, "version": "test"}

@app.get("/")
def root():
    return {"message": "Test API - No Database", "working": True}

if __name__ == "__main__":
    import uvicorn
    import os
    port = int(os.getenv("PORT", "8000"))
    uvicorn.run("main:app", host="0.0.0.0", port=port)