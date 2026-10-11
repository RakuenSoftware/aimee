FROM python:3.13-slim
WORKDIR /app
RUN pip install --no-cache-dir 'cognee[api]==1.6.2'
COPY integrations/cognee/service.py ./service.py
CMD ["python", "-m", "uvicorn", "service:app", "--host", "0.0.0.0", "--port", "8000"]
