
# ESP32 Minimal Modular Display v0.2.0

A secure, production-ready IoT display system with improved error handling, rate limiting, and deployment options.

## Features
- **ESP32** displays content from configurable modules with automatic recovery
- **FastAPI + SQLite API** with rate limiting, input validation, and comprehensive logging
- **Web interface** for device pairing and module configuration
- **Multiple deployment options**: Docker, Render, Fly.io
- **Production security**: Certificate validation, CORS restrictions, secure sessions

## Quick Start

### 1) Local Development
```bash
# Clone and setup
git clone <your-repo>
cd simpDisgpt

# Set up environment
cp .env.example .env
# Edit .env with your configuration

# Run with Docker
docker-compose up --build

# Or run locally
cd api
python -m venv .venv && source .venv/bin/activate
pip install -r requirements.txt
export APP_SECRET="your-secret-key"
uvicorn main:app --host 0.0.0.0 --port 8000
```

Visit http://localhost:8000 to access the web interface.

### 2) Production Deployment

#### Option A: Render (Recommended)
1. Fork this repository
2. Connect to Render and create a new web service
3. Use `render.yaml` for configuration
4. Set environment variables in Render dashboard

#### Option B: Fly.io
```bash
# Install flyctl and login
fly auth login

# Deploy
fly launch --name your-app-name
fly secrets set APP_SECRET="your-secret-key"
fly deploy
```

#### Option C: Docker on VPS
```bash
# Set environment variables
export APP_SECRET="your-secret-key"
export ALLOWED_ORIGINS="https://yourdomain.com"

# Deploy
docker-compose up -d
```

### 3) ESP32 Setup
1. Install required libraries:
   - WiFiManager by tzapu
   - U8g2 by olikraus
   - ArduinoJson

2. Configure firmware:
   ```cpp
   const char* SERVER_BASE = "https://your-domain.com";
   const bool VALIDATE_CERTIFICATES = true; // Set to true for production
   ```

3. Flash to ESP32. Device will create WiFi portal `ESP32-Display-XXXX` if not configured.

### 4) Device Pairing
1. ESP32 displays 6-digit pairing code
2. Visit your web interface and enter the code
3. Choose display module (Text, BTC Price, Weather)
4. Device automatically updates with new content

## Security Features

### Production Hardening
- Rate limiting on all endpoints
- Input validation and sanitization
- CORS restrictions
- Secure cookie settings
- Certificate validation options
- Device token expiration
- Comprehensive logging

### ESP32 Security
- Certificate validation (configurable)
- Automatic token refresh
- Error recovery and retry logic
- Watchdog timer protection
- Secure token storage in NVS

## Configuration

### Environment Variables
```bash
ENVIRONMENT=production                    # Environment mode
APP_SECRET=your-secret-key               # JWT secret (required)
ALLOWED_ORIGINS=https://yourdomain.com   # CORS origins
DB_PATH=/data/app.db                     # Database path
PAIR_TTL_SECONDS=300                     # Pairing code TTL
SESSION_TTL_MINUTES=30                   # Web session TTL
DEVICE_TOKEN_TTL_HOURS=8760              # Device token TTL
```

### ESP32 Configuration
```cpp
const char* SERVER_BASE = "https://your-domain.com";
const bool VALIDATE_CERTIFICATES = true;  // Enable for production
const int POLL_FALLBACK_SEC = 10;         // Polling interval
const int MAX_RETRY_ATTEMPTS = 3;         // HTTP retry attempts
```

## Available Modules

### Text Module
- Display custom messages
- Configurable line length
- Multi-line support

### BTC Price Module
- Real-time Bitcoin price
- 24-hour change percentage
- CoinGecko API (no key required)

### Weather Module
- Current weather conditions
- City-based lookup
- Open-Meteo API (no key required)

## API Endpoints

- `POST /pair/start` - Start device pairing
- `POST /pair/claim` - Claim pairing code
- `POST /device/{device_id}/module` - Configure device module
- `GET /device/config` - Get device configuration
- `GET /healthz` - Health check
- `GET /` - Web interface

## Troubleshooting

### Common Issues
1. **Pairing fails**: Check network connectivity and server URL
2. **Token expired**: Device will automatically re-pair
3. **Module not updating**: Check server logs for API errors
4. **WiFi issues**: Device will show reconnection status

### Logging
- Production logs at INFO level
- Development logs at DEBUG level
- All API requests logged with rate limiting info
- Error tracking with request correlation

## Development

### Adding New Modules
1. Add module type to API validation
2. Implement data fetcher in `main.py`
3. Add rendering logic in `device_config()` endpoint
4. Update web interface with new module options

### Testing
```bash
# Install test dependencies
pip install pytest httpx

# Run tests
pytest tests/
```

## License
MIT License - feel free to use for personal or commercial projects.
