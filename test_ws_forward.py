import asyncio
import websockets
import sys
import time

PROXY_URL = sys.argv[1] if len(sys.argv) > 1 else "ws://127.0.0.1:18808/ws/incppect"

async def single_connect(i, stop_event, results):
    try:
        async with websockets.connect(PROXY_URL, ping_interval=None) as ws:
            results["connected"] += 1
            for j in range(1000):
                if stop_event.is_set():
                    break
                msg = f"c{i}-m{j}"
                await ws.send(msg)
                resp = await asyncio.wait_for(ws.recv(), timeout=5)
                if resp != msg:
                    results["mismatch"] += 1
                else:
                    results["echo_ok"] += 1
    except asyncio.TimeoutError:
        results["timeout"] += 1
    except Exception as e:
        results["errors"].append(str(e))
    finally:
        results["closed"] += 1

async def main():
    stop_event = asyncio.Event()
    results = {
        "connected": 0, "closed": 0, "echo_ok": 0,
        "mismatch": 0, "timeout": 0, "errors": []
    }

    N = 8
    tasks = [asyncio.create_task(single_connect(i, stop_event, results)) for i in range(N)]

    print(f"[{time.strftime('%H:%M:%S')}] {N} connections started, running for 10s...")
    await asyncio.sleep(10)
    print(f"[{time.strftime('%H:%M:%S')}] stopping...")
    stop_event.set()

    await asyncio.gather(*tasks, return_exceptions=True)
    print(f"[{time.strftime('%H:%M:%S')}] done")
    print(f"  connected: {results['connected']}")
    print(f"  closed:    {results['closed']}")
    print(f"  echo ok:   {results['echo_ok']}")
    print(f"  mismatch:  {results['mismatch']}")
    print(f"  timeout:   {results['timeout']}")
    if results["errors"]:
        print(f"  errors:    {results['errors'][:5]}...")

if __name__ == "__main__":
    asyncio.run(main())
