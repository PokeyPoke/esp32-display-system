#!/usr/bin/env python3
import requests
import json

# Test the API endpoint that ESP32 is calling
url = "https://esp32-display-api.onrender.com/pair/start"
payload = {"hardware_uid": "esp32c3:9C:9E:6E:C3:39:80"}

print(f"Testing POST {url}")
print(f"Payload: {payload}")

try:
    response = requests.post(url, json=payload, timeout=30)
    print(f"Status: {response.status_code}")
    print(f"Response: {response.text}")
    print(f"Headers: {dict(response.headers)}")
except Exception as e:
    print(f"Error: {e}")