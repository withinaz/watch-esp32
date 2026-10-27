"""
Serveur de test local pour l'app Assistant (protocole XiaoZhi).

Role : verifier la chaine audio complete de la montre sans passer par le
serveur officiel. Le serveur se comporte comme un perroquet : il enregistre
les trames Opus recues pendant que la montre ecoute, puis les lui renvoie
comme reponse. La montre doit donc se reentendre.

    python mock_xiaozhi.py [port]

A lancer depuis le PC, sur le meme reseau que la montre.
"""
import asyncio
import json
import sys

from websockets.asyncio.server import serve

FRAMES = []
ECHO_LEN = 0


async def handler(ws):
    global ECHO_LEN
    print(f"[+] connexion de {ws.remote_address}", flush=True)

    await ws.send(json.dumps({
        "type": "hello",
        "transport": "websocket",
        "session_id": "0123456789abcdef",
        "audio_params": {"format": "opus", "sample_rate": 24000,
                         "channels": 1, "frame_duration": 60},
    }))

    FRAMES.clear()
    listening = False

    async for msg in ws:
        if isinstance(msg, (bytes, bytearray)):
            FRAMES.append(bytes(msg))
            if len(FRAMES) % 10 == 1:
                print(f"[audio] {len(FRAMES)} trames, derniere={len(msg)} o", flush=True)
            continue

        try:
            j = json.loads(msg)
        except Exception:
            print(f"[?] texte non JSON : {msg[:80]!r}", flush=True)
            continue

        t = j.get("type")
        st = j.get("state")
        print(f"[json] {t}/{st}", flush=True)

        if t == "hello":
            print(f"[json] hello client: {json.dumps(j.get('audio_params'))}", flush=True)

        elif t == "listen" and st == "start":
            FRAMES.clear()
            listening = True
            print("[json] ecoute commencee", flush=True)

        elif t == "listen" and st == "stop":
            listening = False
            n = len(FRAMES)
            print(f"[json] ecoute terminee, {n} trames recues", flush=True)

            # fausse transcription, puis on renvoie l'audio du client
            await ws.send(json.dumps({"type": "stt", "text": f"test {n} trames"}))
            await ws.send(json.dumps({"type": "tts", "state": "start"}))
            await ws.send(json.dumps({"type": "tts", "state": "sentence_start",
                                      "text": "Je te renvoie ta propre voix."}))
            for f in FRAMES:
                await ws.send(f)
                await asyncio.sleep(0.06)
            await ws.send(json.dumps({"type": "tts", "state": "stop"}))
            ECHO_LEN = n
            print(f"[json] renvoye {n} trames", flush=True)

        elif t == "abort":
            print("[json] abort", flush=True)

    print(f"[-] deconnexion (echo={ECHO_LEN})", flush=True)


async def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8765
    async with serve(handler, "0.0.0.0", port, max_size=None) as server:
        print(f"mock xiaozhi en ecoute sur ws://0.0.0.0:{port}/xiaozhi/v1/", flush=True)
        await server.serve_forever()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
