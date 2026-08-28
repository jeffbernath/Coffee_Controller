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


class LoadCellCalibrationRequest(BaseModel):
    known_grams: float


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

    # Keep a combined weight value for the main scale display and for any
    # clients that still use the original weight_g field.  Both load cells
    # must be available before the total is considered valid.
    load_cell_names = ("LOAD_CELL_1_G", "LOAD_CELL_2_G")
    values = [snapshot["io"].get(name) for name in load_cell_names]
    available = [snapshot["available"].get(name, False) for name in load_cell_names]
    if all(available) and all(isinstance(value, (int, float)) for value in values):
        snapshot["weight_g"] = float(values[0]) + float(values[1])
    else:
        snapshot["weight_g"] = None

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


@app.post("/api/tare")
async def tare_scale():
    try:
        sequence = pico.tare_all()
    except ConnectionError as exc:
        raise HTTPException(status_code=503, detail=str(exc)) from exc

    return {"requested": True, "sequence": sequence}


@app.post("/api/load-cell/{channel}/calibrate")
async def calibrate_load_cell(channel: int, request: LoadCellCalibrationRequest):
    if channel not in (1, 2):
        raise HTTPException(status_code=400, detail="INVALID_LOAD_CELL_CHANNEL")
    if request.known_grams <= 0:
        raise HTTPException(status_code=400, detail="KNOWN_WEIGHT_MUST_BE_POSITIVE")

    try:
        sequence = pico.calibrate_load_cell(channel, request.known_grams)
    except ValueError as exc:
        raise HTTPException(status_code=400, detail=str(exc)) from exc
    except ConnectionError as exc:
        raise HTTPException(status_code=503, detail=str(exc)) from exc

    return {
        "requested": True,
        "sequence": sequence,
        "channel": channel,
        "known_grams": request.known_grams,
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
