FROM python:3.13-slim
WORKDIR /app
COPY integrations/discord/requirements.txt ./requirements.txt
RUN pip install --no-cache-dir -r requirements.txt
COPY integrations/discord/bridge.py ./bridge.py
COPY integrations/discord/conversation_memory.py ./conversation_memory.py
ENTRYPOINT ["python", "bridge.py"]
