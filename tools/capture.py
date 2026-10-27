"""
Capture le journal serie de la montre dans un fichier.

    python tools/capture.py COM4 sortie.log 30

Attend que le port se libere (esptool le tient juste apres le flash), puis
lit pendant la duree demandee. Le journal produit se decode ensuite avec
tools/snap.py.
"""
import sys
import time

import serial


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 1

    port = sys.argv[1]
    out = sys.argv[2]
    duration = float(sys.argv[3]) if len(sys.argv) > 3 else 30.0

    # esptool vient de rendre le port : on retente quelques secondes.
    ser = None
    deadline = time.time() + 8.0
    while ser is None and time.time() < deadline:
        try:
            ser = serial.Serial(port, 115200, timeout=0.5)
        except Exception:
            time.sleep(0.2)
    if ser is None:
        print("port %s indisponible" % port)
        return 3

    print("capture de %s pendant %.0f s..." % (port, duration))
    t0 = time.time()
    with open(out, "w", encoding="utf-8", errors="replace", newline="\n") as f:
        while time.time() - t0 < duration:
            data = ser.read(65536)
            if data:
                f.write(data.decode("utf-8", "replace"))
                f.flush()
    ser.close()
    print("ecrit", out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
