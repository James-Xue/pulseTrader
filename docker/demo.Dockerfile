# Demo image: a zero-dependency control-plane engine serving sample payloads.
#
# It exists so a newcomer can see the protocol working without exchange keys,
# a funded account or the C++ toolchain:
#
#   docker build -f docker/demo.Dockerfile -t pulsetrader-demo .
#   docker run --rm -p 8081:8081 pulsetrader-demo
#
# Then point any client at 127.0.0.1:8081. This is a fixture serving fixed
# sample data, not a simulator and not the trading engine.
FROM python:3.12-slim

WORKDIR /app

COPY tools/demo_engine.py /app/demo_engine.py

# The socket binds loopback inside the container; publish the port to reach it.
EXPOSE 8081

CMD ["python3", "-u", "/app/demo_engine.py", "8081"]
