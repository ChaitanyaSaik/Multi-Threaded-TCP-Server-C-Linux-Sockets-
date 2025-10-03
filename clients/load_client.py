# load_client.py
# python3 load_client.py <host> <port> <num_clients>
import socket, sys, time

def run(host, port, nclients):
    socks = []
    for i in range(nclients):
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.settimeout(5)
        try:
            s.connect((host, port))
            socks.append(s)
        except Exception as e:
            print("connect error:", e)
            break
    print(f"Connected {len(socks)}/{nclients}")
    # send a message on each socket
    for idx, s in enumerate(socks):
        try:
            msg = f"hello from client-{idx}\n".encode()
            s.sendall(msg)
        except Exception as e:
            print("send error:", e)
    # read responses
    for idx, s in enumerate(socks[:50]):  # read first 50 responses to verify
        try:
            data = s.recv(4096)
            print("resp", idx, data.decode().strip())
        except Exception as e:
            print("recv error", e)
    # keep sockets open for concurrency test
    print("Sleeping 30s with connections open...")
    time.sleep(30)
    for s in socks:
        s.close()

if __name__ == "__main__":
    if len(sys.argv) < 4:
        print("Usage: python3 load_client.py <host> <port> <num_clients>")
        sys.exit(1)
    run(sys.argv[1], int(sys.argv[2]), int(sys.argv[3]))
