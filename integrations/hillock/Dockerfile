FROM python:3.13-slim-bookworm AS source
RUN apt-get update && apt-get install -y --no-install-recommends git ca-certificates && rm -rf /var/lib/apt/lists/*
RUN git clone https://github.com/roandejager/Hillock.git /opt/hillock && \
    cd /opt/hillock && git checkout --detach 1edd166ead75b85a9ab95cd6ba4faf7011ad567c && \
    test "$(git rev-parse HEAD)" = 1edd166ead75b85a9ab95cd6ba4faf7011ad567c && \
    git rev-parse HEAD > AIMEE_UPSTREAM_REVISION && rm -rf .git
FROM python:3.13-slim-bookworm
COPY --from=source /opt/hillock /opt/hillock
COPY requirements.txt /app/requirements.txt
RUN pip install --no-cache-dir -r /app/requirements.txt && useradd --uid 10001 --create-home hillock
COPY service.py /app/service.py
USER 10001:10001
ENV HILLOCK_HOST=0.0.0.0 PYTHONDONTWRITEBYTECODE=1 PYTHONUNBUFFERED=1
EXPOSE 8097
ENTRYPOINT ["python", "/app/service.py"]
