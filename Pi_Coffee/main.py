from __future__ import annotations

import asyncio
import queue
from contextlib import asynccontextmanager
from pathlib import Path
from typing import Any

from fastapi import FastAPI, HTTPException, Request, WebSocket, WebSocketDisconnect
from fastapi.responses import HTMLResponse
from fastapi.staticfiles import StaticFiles
from fastapi.templating import Jinja2Templates
from pydantic import BaseModel

from pico import IoRegistry, PicoManager

BASE_DIR = Path(__file__).resolve().parent
PROJECT_DIR = BASE_DIR.parent
MANIFEST_PATH = PROJECT_DIR / "config" / "io_manifest.json"

registry = IoRegistry(MANIFEST_PATH)
pico = PicoManager(registry)
websockets: set[WebSocket] = set()


class IoWriteRequest(BaseModel):
    value: bool | int | float


async def broadcast(message: dict[str, Any]) -> None:
    stale: list[WebSocket] = []
    for websocket in list(websockets):
        try:
            await websocket.send_json(message)
        except Exception:
            stale.append(websocket)
    for websocket in stale:
        websockets.discard(websocket)


async def pico_event_pump() -> None:
    while True:
        try:
            event = await asyncio.to_thread(pico.events.get, True, 0.5)
        except queue.Empty:
            continue
        await broadcast(event)


@asynccontextmanager
async def lifespan(app: FastAPI):
    pico.start()
    event_task = asyncio.create_task(pico_event_pump())
    try:
        yield
    finally:
        event_task.cancel()
        pico.stop()


app = FastAPI(lifespan=lifespan)
app.mount("/static", StaticFiles(directory=BASE_DIR / "static"), name="static")
templates = Jinja2Templates(directory=BASE_DIR / "templates")


@app.get("/", response_class=HTMLResponse)
async def home(request: Request):
    return templates.TemplateResponse(request=request, name="index.html")


@app.get("/api/io/manifest")
async def io_manifest():
    return registry.manifest


@app.get("/api/status")
async def status():
    snapshot = pico.snapshot()
    # Keep weight_g for compatibility with the original GUI while the new GUI
    # uses the generic io dictionary.
    snapshot["weight_g"] = snapshot["io"].get("LOAD_CELL_1_G")
    return snapshot


@app.post("/api/io/{io_name}")
async def set_io(io_name: str, request: IoWriteRequest):
    try:
        sequence = pico.set_output(io_name, request.value)
    except ValueError as exc:
        raise HTTPException(status_code=400, detail=str(exc)) from exc
    except ConnectionError as exc:
        raise HTTPException(status_code=503, detail=str(exc)) from exc
    return {"requested": True, "sequence": sequence, "name": io_name}


@app.post("/api/action/{action_name}")
async def run_action(action_name: str):
    try:
        sequence = pico.run_action(action_name)

    except ValueError as exc:
        raise HTTPException(
            status_code=400,
            detail=str(exc)
        ) from exc

    except ConnectionError as exc:
        raise HTTPException(
            status_code=503,
            detail=str(exc)
        ) from exc

    return {
        "requested": True,
        "sequence": sequence,
        "name": action_name
    }

@app.websocket("/ws")
async def websocket_endpoint(websocket: WebSocket):
    await websocket.accept()
    websockets.add(websocket)
    await websocket.send_json({"type": "snapshot", **pico.snapshot()})
    try:
        while True:
            # The browser currently only receives data. Waiting for a message
            # here keeps the connection open and detects client disconnects.
            await websocket.receive_text()
    except WebSocketDisconnect:
        pass
    finally:
        websockets.discard(websocket)
