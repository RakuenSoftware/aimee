"""Pinned Cognee API plus authenticated chunk-only indexing for Aimee retrieval.

Cognee still owns chunking, embeddings, dataset locks, provenance and deletion.
Graph enrichment remains available through the upstream cognify API, but is not
on the latency-sensitive CHUNKS retrieval path.
"""
from cognee import __version__
from cognee.api.client import app
from cognee.api.v1.cognify.cognify import raise_if_cognify_errored
from cognee.modules.pipelines import Task, run_pipeline
from cognee.modules.pipelines.layers.pipeline_execution_mode import run_pipeline_blocking
from cognee.modules.users.methods import get_authenticated_user
from cognee.modules.users.models import User
from cognee.tasks.documents import classify_documents, extract_chunks_from_documents
from cognee.tasks.storage import add_data_points
from fastapi import Depends
from pydantic import BaseModel, Field, field_validator

if __version__ != "1.6.2":
    raise RuntimeError("Aimee chunk pipeline requires Cognee 1.6.2")


class ChunkIndex(BaseModel):
    datasets: list[str] = Field(min_length=1, max_length=4)

    @field_validator("datasets")
    @classmethod
    def validate_names(cls, names):
        if len(set(names)) != len(names) or any(not n.startswith("aimee_") or len(n) > 200 for n in names):
            raise ValueError("unique Aimee dataset names required")
        return names


@app.post("/api/v1/aimee/index-chunks")
async def index_chunks(payload: ChunkIndex, user: User = Depends(get_authenticated_user)):
    tasks = [Task(classify_documents, needs_llm=False),
             Task(extract_chunks_from_documents, max_chunk_size=512, needs_llm=False),
             Task(add_data_points, task_config={"batch_size": 16}, needs_llm=False)]
    result = await run_pipeline_blocking(run_pipeline, datasets=payload.datasets,
        tasks=tasks, pipeline_name="aimee_chunks_v1", user=user,
        incremental_loading=True, data_per_batch=4, needs_llm=False)
    raise_if_cognify_errored(result)
    return result
