const weightElement = document.getElementById("weight");
const picoStatusElement = document.getElementById("pico-status");
const picoIndicatorElement = document.getElementById("pico-indicator");
const tareButton = document.getElementById("tare-button");

async function updateStatus() {
  try {
    const response = await fetch("/api/status");
    if (!response.ok) throw new Error(`HTTP ${response.status}`);
    const data = await response.json();
    weightElement.textContent = `${Number(data.weight_g).toFixed(2)} g`;
    picoStatusElement.textContent = data.pico_online ? "Pico: ONLINE" : "Pico: OFFLINE";
    picoIndicatorElement.className = data.pico_online ? "indicator online" : "indicator offline";
  } catch (error) {
    picoStatusElement.textContent = "Pico: OFFLINE";
    picoIndicatorElement.className = "indicator offline";
    console.error("Status update failed:", error);
  }
}

tareButton.addEventListener("click", async () => {
  try {
    const response = await fetch("/api/tare", { method: "POST" });
    if (!response.ok) throw new Error(`HTTP ${response.status}`);
  } catch (error) {
    console.error("Tare failed:", error);
  }
});

updateStatus();
setInterval(updateStatus, 500);
