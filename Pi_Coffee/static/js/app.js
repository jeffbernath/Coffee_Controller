const picoStatusElement = document.getElementById("pico-status");
const picoIndicatorElement = document.getElementById("pico-indicator");
const loadCellGrid = document.getElementById("load-cell-grid");
const outputControls = document.getElementById("output-controls");
const inputStatus = document.getElementById("input-status");
const commandMessage = document.getElementById("command-message");
const totalWeightElement = document.getElementById("total-weight");
const totalWeightStatusElement = document.getElementById("total-weight-status");
const tareButton = document.getElementById("tare-button");
const calibrationWeightInput = document.getElementById("calibration-weight");
const calibrateCell1Button = document.getElementById("calibrate-cell-1");
const calibrateCell2Button = document.getElementById("calibrate-cell-2");
const calibrationStatusElement = document.getElementById("calibration-status");

let manifest = null;
let picoOnline = false;
const ioState = {};
const ioAvailable = {};
let socket = null;
let reconnectTimer = null;

function formatValue(definition, value) {
  if (value === null || value === undefined) return "--";
  if (definition.data_type === "bool") return value ? "ON" : "OFF";
  const decimals = definition.gui?.decimals ?? 1;
  return Number(value).toFixed(decimals);
}

function updateTotalWeight() {
  const names = ["LOAD_CELL_1_G", "LOAD_CELL_2_G"];
  const ready = names.every((name) =>
    ioAvailable[name] === true && Number.isFinite(Number(ioState[name]))
  );

  if (!ready) {
    totalWeightElement.textContent = "--";
    totalWeightStatusElement.textContent = "Waiting for both calibrated load cells";
    totalWeightStatusElement.classList.add("unavailable");
    return;
  }

  const total = Number(ioState.LOAD_CELL_1_G) + Number(ioState.LOAD_CELL_2_G);
  totalWeightElement.textContent = total.toFixed(2);
  totalWeightStatusElement.textContent = "Live";
  totalWeightStatusElement.classList.remove("unavailable");
}

function showMessage(message, isError = false) {
  commandMessage.textContent = message || "";
  commandMessage.classList.toggle("error", isError);
}

function setPicoStatus(online) {
  picoOnline = Boolean(online);
  picoStatusElement.textContent = picoOnline ? "Pico: ONLINE" : "Pico: OFFLINE";
  picoIndicatorElement.className = picoOnline ? "indicator online" : "indicator offline";
  document.querySelectorAll("[data-requires-pico='true']").forEach((control) => {
    control.disabled = !picoOnline;
  });
}

function definitionByName(name) {
  return manifest?.io.find((item) => item.name === name);
}

function buildLoadCell(definition) {
  const card = document.createElement("article");
  card.className = "weight-panel";
  card.dataset.ioCard = definition.name;

  const label = document.createElement("div");
  label.className = "weight-label";
  label.textContent = definition.gui?.label ?? definition.name;

  const value = document.createElement("div");
  value.className = "weight";
  value.dataset.ioValue = definition.name;
  value.textContent = "--";

  const unit = document.createElement("div");
  unit.className = "weight-unit";
  unit.textContent = definition.units ?? "";

  const state = document.createElement("div");
  state.className = "sensor-state";
  state.dataset.ioAvailability = definition.name;
  state.textContent = "Waiting for Pico";

  card.append(label, value, unit, state);
  loadCellGrid.appendChild(card);
}

function buildDigitalInput(definition) {
  const row = document.createElement("div");
  row.className = "status-item";

  const label = document.createElement("span");
  label.className = "status-label";
  label.textContent = definition.gui?.label ?? definition.name;

  const value = document.createElement("span");
  value.className = "status-value unknown";
  value.dataset.ioValue = definition.name;
  value.textContent = "--";

  row.append(label, value);
  inputStatus.appendChild(row);
}

function buildToggle(definition) {
  const row = document.createElement("div");
  row.className = "control-row";

  const copy = document.createElement("div");
  const label = document.createElement("div");
  label.className = "control-label";
  label.textContent = definition.gui?.label ?? definition.name;
  const confirmed = document.createElement("div");
  confirmed.className = "control-confirmed";
  confirmed.dataset.ioConfirmed = definition.name;
  confirmed.textContent = "Confirmed: --";
  copy.append(label, confirmed);

  const button = document.createElement("button");
  button.type = "button";
  button.className = "toggle-button";
  button.dataset.ioToggle = definition.name;
  button.dataset.requiresPico = "true";
  button.disabled = true;
  button.textContent = "TURN ON";
  button.addEventListener("click", async () => {
    const requested = !Boolean(ioState[definition.name]);
    await requestIo(definition.name, requested);
  });

  row.append(copy, button);
  outputControls.appendChild(row);
}

function buildSliderNumber(definition) {
  const row = document.createElement("div");
  row.className = "control-row slider-row";

  const heading = document.createElement("div");
  heading.className = "slider-heading";
  const label = document.createElement("label");
  label.className = "control-label";
  label.htmlFor = `number-${definition.name}`;
  label.textContent = definition.gui?.label ?? definition.name;
  const confirmed = document.createElement("div");
  confirmed.className = "control-confirmed";
  confirmed.dataset.ioConfirmed = definition.name;
  confirmed.textContent = "Confirmed: --";
  heading.append(label, confirmed);

  const controls = document.createElement("div");
  controls.className = "slider-controls";

  const slider = document.createElement("input");
  slider.type = "range";
  slider.min = definition.min;
  slider.max = definition.max;
  slider.step = definition.step ?? 1;
  slider.value = definition.default ?? definition.min ?? 0;
  slider.dataset.ioSlider = definition.name;
  slider.dataset.requiresPico = "true";
  slider.disabled = true;

  const numberWrap = document.createElement("div");
  numberWrap.className = "number-wrap";
  const number = document.createElement("input");
  number.type = "number";
  number.id = `number-${definition.name}`;
  number.min = definition.min;
  number.max = definition.max;
  number.step = definition.step ?? 1;
  number.value = slider.value;
  number.dataset.ioNumber = definition.name;
  number.dataset.requiresPico = "true";
  number.disabled = true;
  const unit = document.createElement("span");
  unit.textContent = definition.units ?? "";
  numberWrap.append(number, unit);

  slider.addEventListener("input", () => {
    number.value = slider.value;
  });
  slider.addEventListener("change", () => requestIo(definition.name, Number(slider.value)));
  number.addEventListener("change", async () => {
    const min = Number(definition.min);
    const max = Number(definition.max);
    let value = Number(number.value);
    if (!Number.isFinite(value)) value = Number(definition.default ?? min);
    value = Math.min(max, Math.max(min, value));
    number.value = value;
    slider.value = value;
    await requestIo(definition.name, value);
  });

  controls.append(slider, numberWrap);
  row.append(heading, controls);
  outputControls.appendChild(row);
}

function buildActionButton(definition) {
  const button = document.createElement("button");

  button.className = "secondary-button";
  button.type = "button";
  button.textContent = definition.gui?.label ?? definition.name;
  button.dataset.requiresPico = "true";
  button.disabled = !picoOnline;

  button.addEventListener("click", async () => {
    await requestAction(definition.name);
  });

  if (definition.gui?.group === "load_cells") {
    document.getElementById("load-cell-actions").appendChild(button);
  } else {
    outputControls.appendChild(button);
  }
}

function buildUi() {
  loadCellGrid.replaceChildren();
  outputControls.replaceChildren();
  inputStatus.replaceChildren();

  for (const definition of manifest.io) {
    if (definition.gui?.show === false) continue;
    const widget = definition.gui?.widget;
    if (
      definition.type === "action" &&
      definition.direction === "command" &&
      widget === "button"
    ) {
      if (definition.name !== "TARE_BOTH") buildActionButton(definition);
    } else if (definition.type === "load_cell" && definition.direction === "input") {
      buildLoadCell(definition);
    } else if (definition.direction === "input" && widget === "status") {
      buildDigitalInput(definition);
    } else if (definition.direction === "output" && widget === "toggle") {
      buildToggle(definition);
    } else if (definition.direction === "output" && widget === "slider_number") {
      buildSliderNumber(definition);
    }
  }
  setPicoStatus(picoOnline);
}

function updateIo(name, value, available = true) {
  const definition = definitionByName(name);
  if (!definition) return;

  ioState[name] = value;
  ioAvailable[name] = available;

  document.querySelectorAll(`[data-io-value='${name}']`).forEach((element) => {
    if (!available) {
      element.textContent = "--";
      element.classList.add("unknown");
      element.classList.remove("active");
      return;
    }

    if (definition.data_type === "bool") {
      const trueText = definition.gui?.true_text ?? "ON";
      const falseText = definition.gui?.false_text ?? "OFF";
      element.textContent = value ? trueText : falseText;
      element.classList.toggle("active", Boolean(value));
      element.classList.remove("unknown");
    } else {
      element.textContent = formatValue(definition, value);
    }
  });

  document.querySelectorAll(`[data-io-availability='${name}']`).forEach((element) => {
    element.textContent = available ? "Live" : "Load-cell driver not configured";
    element.classList.toggle("unavailable", !available);
  });

  document.querySelectorAll(`[data-io-confirmed='${name}']`).forEach((element) => {
    const formatted = available ? formatValue(definition, value) : "--";
    const unit = definition.units ? ` ${definition.units}` : "";
    element.textContent = `Confirmed: ${formatted}${formatted === "--" ? "" : unit}`;
  });

  const toggle = document.querySelector(`[data-io-toggle='${name}']`);
  if (toggle) {
    const on = Boolean(value);
    toggle.textContent = on ? "TURN OFF" : "TURN ON";
    toggle.classList.toggle("active", on);
  }

  const slider = document.querySelector(`[data-io-slider='${name}']`);
  const number = document.querySelector(`[data-io-number='${name}']`);
  if (available && value !== null && value !== undefined) {
    if (slider) slider.value = value;
    if (number && document.activeElement !== number) number.value = value;
  }

  if (name === "LOAD_CELL_1_G" || name === "LOAD_CELL_2_G") {
    updateTotalWeight();
  }
}

function applySnapshot(snapshot) {
  setPicoStatus(snapshot.pico_online);
  for (const [name, value] of Object.entries(snapshot.io ?? {})) {
    const available = snapshot.available?.[name] ?? value !== null;
    updateIo(name, value, available);
  }
}

async function requestIo(name, value) {
  showMessage(`Requesting ${name}...`);
  try {
    const response = await fetch(`/api/io/${encodeURIComponent(name)}`, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ value }),
    });
    const data = await response.json();
    if (!response.ok) throw new Error(data.detail ?? `HTTP ${response.status}`);
    showMessage(`Request sent. Waiting for Pico confirmation (command ${data.sequence}).`);
  } catch (error) {
    showMessage(error.message ?? "I/O request failed", true);
  }
}

async function refreshStatus() {
  try {
    const response = await fetch("/api/status");
    if (!response.ok) throw new Error(`HTTP ${response.status}`);
    applySnapshot(await response.json());
  } catch (error) {
    setPicoStatus(false);
  }
}

function connectWebSocket() {
  if (socket && (socket.readyState === WebSocket.OPEN || socket.readyState === WebSocket.CONNECTING)) return;
  const protocol = window.location.protocol === "https:" ? "wss:" : "ws:";
  socket = new WebSocket(`${protocol}//${window.location.host}/ws`);

  socket.onmessage = (event) => {
    const message = JSON.parse(event.data);
    if (message.type === "snapshot") {
      applySnapshot(message);
    } else if (message.type === "pico_status") {
      setPicoStatus(message.online);
    } else if (message.type === "io") {
      updateIo(message.name, message.value, true);
    } else if (message.type === "io_status") {
      updateIo(message.name, ioState[message.name], message.available);
    } else if (message.type === "command_error") {
      showMessage(`${message.target}: ${message.error}`, true);
    } else if (message.type === "calibration") {
      calibrationStatusElement.textContent = `Cell ${message.channel}: ${Number(message.counts_per_gram).toFixed(6)} counts/g`;
      showMessage(`Load cell ${message.channel} calibrated.`);
    } else if (message.type === "ack") {
      showMessage(`Pico accepted command ${message.sequence}.`);
    }
  };

  socket.onclose = () => {
    socket = null;
    clearTimeout(reconnectTimer);
    reconnectTimer = setTimeout(connectWebSocket, 1000);
  };

  socket.onerror = () => socket.close();
}

async function requestAction(name) {
  showMessage(`Requesting ${name}...`);

  try {
    const response = await fetch(
      `/api/action/${encodeURIComponent(name)}`,
      {
        method: "POST"
      }
    );

    const data = await response.json();

    if (!response.ok) {
      throw new Error(data.detail ?? `HTTP ${response.status}`);
    }

    showMessage(
      `${name} request sent. Waiting for Pico confirmation (command ${data.sequence}).`
    );

  } catch (error) {
    showMessage(error.message ?? `${name} failed`, true);
  }
}

async function requestTare() {
  showMessage("Taring scale...");
  try {
    const response = await fetch("/api/tare", { method: "POST" });
    const data = await response.json();
    if (!response.ok) throw new Error(data.detail ?? `HTTP ${response.status}`);
    showMessage(`Tare request sent. Waiting for Pico confirmation (command ${data.sequence}).`);
  } catch (error) {
    showMessage(error.message ?? "Tare failed", true);
  }
}

async function requestCalibration(channel) {
  const knownGrams = Number(calibrationWeightInput.value);
  if (!Number.isFinite(knownGrams) || knownGrams <= 0) {
    showMessage("Enter a valid known calibration weight.", true);
    return;
  }

  showMessage(`Calibrating load cell ${channel} with ${knownGrams.toFixed(1)} g...`);
  try {
    const response = await fetch(`/api/load-cell/${channel}/calibrate`, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ known_grams: knownGrams }),
    });
    const data = await response.json();
    if (!response.ok) throw new Error(data.detail ?? `HTTP ${response.status}`);
    showMessage(`Calibration request sent for load cell ${channel} (command ${data.sequence}).`);
  } catch (error) {
    showMessage(error.message ?? `Load cell ${channel} calibration failed`, true);
  }
}

function installScaleControls() {
  tareButton.addEventListener("click", requestTare);
  calibrateCell1Button.addEventListener("click", () => requestCalibration(1));
  calibrateCell2Button.addEventListener("click", () => requestCalibration(2));
}

async function init() {
  const response = await fetch("/api/io/manifest");
  if (!response.ok) throw new Error(`Unable to load I/O manifest: HTTP ${response.status}`);
  manifest = await response.json();
  for (const definition of manifest.io) {
    ioState[definition.name] = null;
    ioAvailable[definition.name] = definition.type !== "load_cell";
  }
  buildUi();
  installScaleControls();
  updateTotalWeight();
  await refreshStatus();
  connectWebSocket();
  setInterval(refreshStatus, 2000);
}

init().catch((error) => showMessage(error.message, true));
