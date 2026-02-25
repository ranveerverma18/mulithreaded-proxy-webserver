from fastapi import FastAPI
import socket
import json
app = FastAPI(title="Proxy Control API")

CONTROL_HOST = "127.0.0.1"
CONTROL_PORT = 7070


def send_control_command(cmd: str) -> str:
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.connect((CONTROL_HOST, CONTROL_PORT))
    s.sendall((cmd + "\n").encode())
    data = s.recv(4096).decode()
    s.close()
    return data


@app.get("/stats")
def stats():
    raw = send_control_command("STATS")

    try:
        parsed = json.loads(raw)
        return parsed
    except json.JSONDecodeError:
        return {
            "error": "Invalid JSON from control plane",
            "raw": raw
        }


@app.post("/cache/clear")
def clear_cache():
    response = send_control_command("CLEAR_CACHE")
    return {"status": response}


@app.post("/proxy/shutdown")
def shutdown_proxy():
    response = send_control_command("SHUTDOWN")
    return {"status": response}