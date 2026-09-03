const picoStatusElement = document.getElementById("pico-status");
const picoIndicatorElement = document.getElementById("pico-indicator");
const loadCellGrid = document.getElementById("load-cell-grid");
const temperatureGrid = document.getElementById("temperature-grid");
const outputControls = document.getElementById("output-controls");
const inputStatus = document.getElementById("input-status");
const commandMessage = document.getElementById("command-message");
const totalWeightElement = document.getElementById("total-weight");
const totalWeightStatusElement = document.getElementById("total-weight-status");
const tareButton = document.getElementById("tare-button");
const calibrationWeightInput = document.getElementById("calibration-weight");
const calibrateScaleButton = document.getElementById("calibrate-scale");
const calibrationStatusElement = document.getElementById("calibration-status");

let manifest = null;
let picoOnline = false;
const ioState = {};
const ioAvailable = {};
let socket = null;
let reconnectTimer = null;


// -----------------------------------------------------------------------------
// Touch keyboard / keypad
// -----------------------------------------------------------------------------
let touchInputTarget = null;
let touchInputOriginalValue = "";
let touchInputDraft = "";
let touchKeyboardShift = false;
let touchKeyboardOverlay = null;
let touchKeyboardPanel = null;
let touchKeyboardTitle = null;
let touchKeyboardDisplay = null;
let touchKeyboardKeys = null;

function isTouchKeyboardInput(element) {
  if (!(element instanceof HTMLInputElement)) return false;
  if (element.disabled || element.readOnly) return false;
  if (element.dataset.touchKeyboard === "off") return false;

  return ["number", "text", "search", "email", "tel", "password"].includes(element.type);
}

function touchKeyboardMode(input) {
  return input.type === "number" ? "number" : "text";
}

function numberInputAllowsDecimal(input) {
  if (input.type !== "number") return false;
  if (!input.step || input.step === "any") return true;
  const step = Number(input.step);
  return !Number.isInteger(step);
}

function numberInputAllowsNegative(input) {
  if (input.type !== "number") return false;
  if (input.min === "") return true;
  const min = Number(input.min);
  return Number.isFinite(min) && min < 0;
}

function createTouchKey(label, value = label, className = "") {
  const button = document.createElement("button");
  button.type = "button";
  button.className = `touch-key ${className}`.trim();
  button.textContent = label;
  button.dataset.touchKeyValue = value;
  return button;
}

function buildNumberKeys(input) {
  const fragment = document.createDocumentFragment();
  const keys = ["1", "2", "3", "4", "5", "6", "7", "8", "9"];
  keys.forEach((key) => fragment.appendChild(createTouchKey(key)));

  if (numberInputAllowsDecimal(input)) {
    fragment.appendChild(createTouchKey(".", "."));
  } else if (numberInputAllowsNegative(input)) {
    fragment.appendChild(createTouchKey("−", "-"));
  } else {
    fragment.appendChild(createTouchKey("Clear", "CLEAR", "touch-key-wide touch-key-secondary"));
  }

  fragment.appendChild(createTouchKey("0"));
  fragment.appendChild(createTouchKey("⌫", "BACKSPACE", "touch-key-secondary"));

  if (numberInputAllowsDecimal(input) && numberInputAllowsNegative(input)) {
    fragment.appendChild(createTouchKey("−", "-", "touch-key-secondary"));
  }

  return fragment;
}

function buildTextKeys(input) {
  const fragment = document.createDocumentFragment();
  const rows = [
    "QWERTYUIOP",
    "ASDFGHJKL",
    "ZXCVBNM",
  ];

  rows.forEach((row, rowIndex) => {
    const rowElement = document.createElement("div");
    rowElement.className = "touch-key-row";

    if (rowIndex === 2) {
      rowElement.appendChild(createTouchKey("Shift", "SHIFT", "touch-key-secondary touch-key-shift"));
    }

    [...row].forEach((character) => {
      const visible = touchKeyboardShift ? character : character.toLowerCase();
      rowElement.appendChild(createTouchKey(visible, visible));
    });

    if (rowIndex === 2) {
      rowElement.appendChild(createTouchKey("⌫", "BACKSPACE", "touch-key-secondary"));
    }

    fragment.appendChild(rowElement);
  });

  const utilityRow = document.createElement("div");
  utilityRow.className = "touch-key-row touch-key-row-utility";

  if (input.type === "email") {
    utilityRow.appendChild(createTouchKey("@", "@", "touch-key-secondary"));
    utilityRow.appendChild(createTouchKey(".", ".", "touch-key-secondary"));
  }

  utilityRow.appendChild(createTouchKey("Space", " ", "touch-key-space"));
  utilityRow.appendChild(createTouchKey("Clear", "CLEAR", "touch-key-secondary"));
  fragment.appendChild(utilityRow);

  return fragment;
}

function renderTouchKeyboard() {
  if (!touchInputTarget || !touchKeyboardKeys) return;

  touchKeyboardDisplay.textContent = touchInputTarget.type === "password"
    ? "•".repeat(touchInputDraft.length)
    : (touchInputDraft || " ");

  touchKeyboardKeys.replaceChildren();
  if (touchKeyboardMode(touchInputTarget) === "number") {
    touchKeyboardKeys.className = "touch-keyboard-keys numeric";
    touchKeyboardKeys.appendChild(buildNumberKeys(touchInputTarget));
  } else {
    touchKeyboardKeys.className = "touch-keyboard-keys text";
    touchKeyboardKeys.appendChild(buildTextKeys(touchInputTarget));
  }
}

function openTouchKeyboard(input) {
  if (!isTouchKeyboardInput(input)) return;

  touchInputTarget = input;
  touchInputOriginalValue = input.value ?? "";
  // Numeric fields start blank so the user can immediately enter a new value.
  // Cancel still restores the value that was present before the keypad opened.
  touchInputDraft = touchKeyboardMode(input) === "number"
    ? ""
    : touchInputOriginalValue;
  touchKeyboardShift = false;

  const label = input.labels?.[0]?.textContent?.trim();
  touchKeyboardTitle.textContent = label || input.getAttribute("aria-label") || "Enter value";
  touchKeyboardPanel.dataset.mode = touchKeyboardMode(input);
  touchKeyboardOverlay.hidden = false;
  document.body.classList.add("touch-keyboard-open");
  renderTouchKeyboard();
}

function closeTouchKeyboard(commit) {
  if (!touchInputTarget) return;

  const input = touchInputTarget;

  if (commit) {
    let nextValue = touchInputDraft;

    if (input.type === "number") {
      if (nextValue.trim() === "") {
        showMessage("Enter a valid number.", true);
        return;
      }

      const numeric = Number(nextValue);
      if (!Number.isFinite(numeric)) {
        showMessage("Enter a valid number.", true);
        return;
      }

      let clamped = numeric;
      if (input.min !== "" && Number.isFinite(Number(input.min))) {
        clamped = Math.max(Number(input.min), clamped);
      }
      if (input.max !== "" && Number.isFinite(Number(input.max))) {
        clamped = Math.min(Number(input.max), clamped);
      }
      nextValue = String(clamped);
    }

    input.value = nextValue;
    input.dispatchEvent(new Event("input", { bubbles: true }));
    input.dispatchEvent(new Event("change", { bubbles: true }));
  } else {
    input.value = touchInputOriginalValue;
  }

  touchKeyboardOverlay.hidden = true;
  document.body.classList.remove("touch-keyboard-open");
  touchInputTarget = null;
  touchInputDraft = "";
}

function handleTouchKey(value) {
  if (!touchInputTarget) return;

  if (value === "BACKSPACE") {
    touchInputDraft = touchInputDraft.slice(0, -1);
  } else if (value === "CLEAR") {
    touchInputDraft = "";
  } else if (value === "SHIFT") {
    touchKeyboardShift = !touchKeyboardShift;
    renderTouchKeyboard();
    return;
  } else if (touchInputTarget.type === "number") {
    if (value === ".") {
      if (!touchInputDraft.includes(".")) {
        touchInputDraft = touchInputDraft === "" || touchInputDraft === "-"
          ? `${touchInputDraft}0.`
          : `${touchInputDraft}.`;
      }
    } else if (value === "-") {
      touchInputDraft = touchInputDraft.startsWith("-")
        ? touchInputDraft.slice(1)
        : `-${touchInputDraft}`;
    } else {
      touchInputDraft += value;
    }
  } else {
    touchInputDraft += value;
    if (touchKeyboardShift && /^[A-Z]$/.test(value)) {
      touchKeyboardShift = false;
    }
  }

  renderTouchKeyboard();
}

function installTouchKeyboard() {
  touchKeyboardOverlay = document.createElement("div");
  touchKeyboardOverlay.className = "touch-keyboard-overlay";
  touchKeyboardOverlay.hidden = true;
  touchKeyboardOverlay.innerHTML = `
    <section class="touch-keyboard-panel" role="dialog" aria-modal="true" aria-labelledby="touch-keyboard-title">
      <div class="touch-keyboard-header">
        <div>
          <p class="eyebrow">TOUCH INPUT</p>
          <h2 id="touch-keyboard-title">Enter value</h2>
        </div>
        <button type="button" class="touch-keyboard-close" data-touch-action="cancel" aria-label="Cancel">×</button>
      </div>
      <div class="touch-keyboard-display" aria-live="polite"></div>
      <div class="touch-keyboard-keys"></div>
      <div class="touch-keyboard-actions">
        <button type="button" class="secondary-button touch-cancel" data-touch-action="cancel">Cancel</button>
        <button type="button" class="secondary-button touch-ok" data-touch-action="ok">OK</button>
      </div>
    </section>`;

  document.body.appendChild(touchKeyboardOverlay);
  touchKeyboardPanel = touchKeyboardOverlay.querySelector(".touch-keyboard-panel");
  touchKeyboardTitle = touchKeyboardOverlay.querySelector("#touch-keyboard-title");
  touchKeyboardDisplay = touchKeyboardOverlay.querySelector(".touch-keyboard-display");
  touchKeyboardKeys = touchKeyboardOverlay.querySelector(".touch-keyboard-keys");

  touchKeyboardOverlay.addEventListener("click", (event) => {
    const key = event.target.closest("[data-touch-key-value]");
    if (key) {
      handleTouchKey(key.dataset.touchKeyValue);
      return;
    }

    const action = event.target.closest("[data-touch-action]")?.dataset.touchAction;
    if (action === "ok") closeTouchKeyboard(true);
    if (action === "cancel") closeTouchKeyboard(false);
  });

  document.addEventListener("pointerdown", (event) => {
    const input = event.target.closest("input");
    if (!isTouchKeyboardInput(input)) return;

    // Keep Chromium / the desktop from opening a second on-screen keyboard.
    event.preventDefault();
    input.setAttribute("inputmode", "none");
    input.blur();
    openTouchKeyboard(input);
  });

  document.addEventListener("keydown", (event) => {
    if (touchKeyboardOverlay.hidden) return;
    if (event.key === "Escape") closeTouchKeyboard(false);
    if (event.key === "Enter") closeTouchKeyboard(true);
  });
}

function formatValue(definition, value) {
  if (value === null || value === undefined) return "--";
  if (definition.data_type === "bool") return value ? "ON" : "OFF";

  if (definition.type === "load_cell") {
    return Number(value).toFixed(1);
  }

  const decimals = definition.gui?.decimals ?? 1;
  return Number(value).toFixed(decimals);
}

function unavailableText(definition) {
  if (definition.type === "load_cell") return "Waiting for scale calibration";
  if (definition.driver === "max31865") return "RTD unavailable - check wiring";
  return "Unavailable";
}

function max31865FaultText(value) {
  const code = Number(value);
  if (!Number.isFinite(code)) return "--";
  const byte = Math.max(0, Math.min(255, Math.trunc(code)));
  if (byte === 0) return "OK";

  const faults = [];
  if (byte & 0x80) faults.push("RTD HIGH");
  if (byte & 0x40) faults.push("RTD LOW");
  if (byte & 0x20) faults.push("REFIN HIGH");
  if (byte & 0x10) faults.push("REFIN LOW");
  if (byte & 0x08) faults.push("RTDIN LOW");
  if (byte & 0x04) faults.push("OV/UV");

  const label = faults.length ? faults.join(" / ") : "FAULT";
  return `${label} (0x${byte.toString(16).toUpperCase().padStart(2, "0")})`;
}

function updateTotalWeight() {
  const names = ["LOAD_CELL_1_G", "LOAD_CELL_2_G"];
  const ready = names.every((name) =>
    ioAvailable[name] === true && Number.isFinite(Number(ioState[name]))
  );

  if (!ready) {
    totalWeightElement.textContent = "--";
    totalWeightStatusElement.textContent = "Waiting for scale calibration";
    totalWeightStatusElement.classList.add("unavailable");
    return;
  }

  const total = Number(ioState.LOAD_CELL_1_G) + Number(ioState.LOAD_CELL_2_G);
  totalWeightElement.textContent = total.toFixed(1);
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

function buildTemperatureSensor(definition) {
  const card = document.createElement("article");
  card.className = "temperature-card primary";

  const label = document.createElement("div");
  label.className = "temperature-label";
  label.textContent = definition.gui?.label ?? definition.name;

  const valueRow = document.createElement("div");
  valueRow.className = "temperature-value-row";

  const value = document.createElement("div");
  value.className = "temperature-value";
  value.dataset.ioValue = definition.name;
  value.textContent = "--";

  const unit = document.createElement("div");
  unit.className = "temperature-unit";
  unit.textContent = definition.units ?? "°C";
  valueRow.append(value, unit);

  const fahrenheit = document.createElement("div");
  fahrenheit.className = "temperature-secondary";
  fahrenheit.dataset.temperatureF = definition.name;
  fahrenheit.textContent = "-- °F";

  const state = document.createElement("div");
  state.className = "sensor-state";
  state.dataset.ioAvailability = definition.name;
  state.textContent = "Waiting for Pico";

  card.append(label, valueRow, fahrenheit, state);
  temperatureGrid.appendChild(card);
}

function buildSensorValue(definition) {
  const card = document.createElement("article");
  card.className = "temperature-card";

  const label = document.createElement("div");
  label.className = "temperature-label";
  label.textContent = definition.gui?.label ?? definition.name;

  const valueRow = document.createElement("div");
  valueRow.className = "diagnostic-value-row";
  const value = document.createElement("div");
  value.className = "diagnostic-value";
  value.dataset.ioValue = definition.name;
  value.textContent = "--";
  const unit = document.createElement("span");
  unit.className = "diagnostic-unit";
  unit.textContent = definition.units ?? "";
  valueRow.append(value, unit);

  const state = document.createElement("div");
  state.className = "sensor-state";
  state.dataset.ioAvailability = definition.name;
  state.textContent = "Waiting for Pico";

  card.append(label, valueRow, state);
  temperatureGrid.appendChild(card);
}

function buildFaultStatus(definition) {
  const card = document.createElement("article");
  card.className = "temperature-card";

  const label = document.createElement("div");
  label.className = "temperature-label";
  label.textContent = definition.gui?.label ?? definition.name;

  const value = document.createElement("div");
  value.className = "fault-status unknown";
  value.dataset.ioValue = definition.name;
  value.dataset.faultStatus = definition.name;
  value.textContent = "--";

  const state = document.createElement("div");
  state.className = "sensor-state";
  state.dataset.ioAvailability = definition.name;
  state.textContent = "Waiting for Pico";

  card.append(label, value, state);
  temperatureGrid.appendChild(card);
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
  temperatureGrid.replaceChildren();
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
    } else if (definition.direction === "input" && widget === "temperature") {
      buildTemperatureSensor(definition);
    } else if (definition.direction === "input" && widget === "sensor_value") {
      buildSensorValue(definition);
    } else if (definition.direction === "input" && widget === "fault_status") {
      buildFaultStatus(definition);
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
      element.classList.remove("active", "fault", "ok");
      return;
    }

    if (definition.gui?.widget === "fault_status") {
      const code = Number(value);
      element.textContent = max31865FaultText(code);
      element.classList.toggle("fault", Number.isFinite(code) && code !== 0);
      element.classList.toggle("ok", Number.isFinite(code) && code === 0);
      element.classList.remove("unknown");
    } else if (definition.data_type === "bool") {
      const trueText = definition.gui?.true_text ?? "ON";
      const falseText = definition.gui?.false_text ?? "OFF";
      element.textContent = value ? trueText : falseText;
      element.classList.toggle("active", Boolean(value));
      element.classList.remove("unknown");
    } else {
      element.textContent = formatValue(definition, value);
      element.classList.remove("unknown");
    }
  });

  document.querySelectorAll(`[data-io-availability='${name}']`).forEach((element) => {
    element.textContent = available ? "Live" : unavailableText(definition);
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

  if (name === "BOILER_TEMP_C") {
    document.querySelectorAll(`[data-temperature-f='${name}']`).forEach((element) => {
      if (!available || !Number.isFinite(Number(value))) {
        element.textContent = "-- °F";
      } else {
        const fahrenheit = Number(value) * 9 / 5 + 32;
        element.textContent = `${fahrenheit.toFixed(1)} °F`;
      }
    });
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
      calibrationStatusElement.textContent = `Scale: ${Number(message.counts_per_gram).toFixed(6)} counts/g`;
      showMessage("Scale calibrated.");
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

async function requestCalibration() {
  const knownGrams = Number(calibrationWeightInput.value);
  if (!Number.isFinite(knownGrams) || knownGrams <= 0) {
    showMessage("Enter a valid known calibration weight.", true);
    return;
  }

  showMessage(`Calibrating scale with ${knownGrams.toFixed(1)} g...`);
  try {
    const response = await fetch("/api/scale/calibrate", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ known_grams: knownGrams }),
    });
    const data = await response.json();
    if (!response.ok) throw new Error(data.detail ?? `HTTP ${response.status}`);
    showMessage(`Scale calibration request sent (command ${data.sequence}).`);
  } catch (error) {
    showMessage(error.message ?? "Scale calibration failed", true);
  }
}

function installScaleControls() {
  tareButton.addEventListener("click", requestTare);
  calibrateScaleButton.addEventListener("click", requestCalibration);
}

async function init() {
  const response = await fetch("/api/io/manifest");
  if (!response.ok) throw new Error(`Unable to load I/O manifest: HTTP ${response.status}`);
  manifest = await response.json();
  for (const definition of manifest.io) {
    ioState[definition.name] = null;
    ioAvailable[definition.name] = definition.type !== "load_cell" && definition.driver !== "max31865";
  }
  buildUi();
  installScaleControls();
  updateTotalWeight();
  await refreshStatus();
  connectWebSocket();
  setInterval(refreshStatus, 2000);
}

installTouchKeyboard();

init().catch((error) => showMessage(error.message, true));
