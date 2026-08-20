from fastapi import FastAPI
from fastapi.responses import HTMLResponse

app = FastAPI()


@app.get("/", response_class=HTMLResponse)
async def home():
    return """
    <!DOCTYPE html>
    <html>
    <head>
        <title>CoffeeBar</title>
        <meta name="viewport" content="width=device-width, initial-scale=1">
    </head>
    <body style="
        font-family: Arial, sans-serif;
        text-align: center;
        padding-top: 60px;
    ">
        <h1>Coffee Controller</h1>
        <h2>Weight</h2>

        <div style="
            font-size: 72px;
            font-weight: bold;
        ">
            0.00 g
        </div>

        <br>

        <button style="
            font-size: 28px;
            padding: 20px 50px;
        ">
            TARE
        </button>

        <p style="font-size: 22px;">
            Pico: OFFLINE
        </p>
    </body>
    </html>
    """
