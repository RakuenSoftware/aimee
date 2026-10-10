FROM python:3.13-slim
WORKDIR /app
COPY integrations/discord/requirements.txt ./requirements.txt
RUN pip install --no-cache-dir -r requirements.txt
COPY integrations/discord/bridge.py integrations/discord/behavior.py integrations/discord/workflow_chat.py ./
ENTRYPOINT ["python", "bridge.py"]
