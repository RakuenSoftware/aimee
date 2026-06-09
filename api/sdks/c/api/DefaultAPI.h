#include <stdlib.h>
#include <stdio.h>
#include "../include/apiClient.h"
#include "../include/list.h"
#include "../external/cJSON.h"
#include "../include/keyValuePair.h"
#include "../include/binary.h"
#include "../model/active_release_response.h"
#include "../model/artifact_links_response.h"
#include "../model/artifact_response.h"
#include "../model/blast_radius_response.h"
#include "../model/capabilities_response.h"
#include "../model/code_build_request.h"
#include "../model/code_build_response.h"
#include "../model/code_callers_response.h"
#include "../model/code_find_response.h"
#include "../model/code_project_stats_response.h"
#include "../model/code_projects_response.h"
#include "../model/code_scan_request.h"
#include "../model/code_scan_response.h"
#include "../model/code_search_response.h"
#include "../model/code_structure_response.h"
#include "../model/code_update_request.h"
#include "../model/code_update_response.h"
#include "../model/create_release_request.h"
#include "../model/create_release_response.h"
#include "../model/doc_ingest_response.h"
#include "../model/doc_metadata_response.h"
#include "../model/docs_manifest_request.h"
#include "../model/docs_manifest_response.h"
#include "../model/drain_request.h"
#include "../model/drain_response.h"
#include "../model/entity_profile_response.h"
#include "../model/entity_search_request.h"
#include "../model/entity_search_response.h"
#include "../model/error_response.h"
#include "../model/health_response.h"
#include "../model/ingest_request.h"
#include "../model/ingest_response.h"
#include "../model/ingest_status_response.h"
#include "../model/job_status_response.h"
#include "../model/maintenance_clear_request.h"
#include "../model/maintenance_clear_response.h"
#include "../model/maintenance_reconcile_request.h"
#include "../model/maintenance_reconcile_response.h"
#include "../model/maintenance_repair_request.h"
#include "../model/maintenance_repair_response.h"
#include "../model/object.h"
#include "../model/pipeline_status_response.h"
#include "../model/review_accept_request.h"
#include "../model/review_queue_response.h"
#include "../model/review_reject_request.h"
#include "../model/rollback_request.h"
#include "../model/search_request.h"
#include "../model/search_response.h"
#include "../model/version_response.h"
#include "../model/workers_response.h"


// Delete a staged document
//
void
DefaultAPI_deleteDoc(apiClient_t *apiClient, long id);


// Get the currently active corpus release
//
active_release_response_t*
DefaultAPI_getActiveRelease(apiClient_t *apiClient);


// Retrieve an artifact by UUID
//
artifact_response_t*
DefaultAPI_getArtifact(apiClient_t *apiClient, char *id);


// Retrieve outgoing links from an artifact
//
artifact_links_response_t*
DefaultAPI_getArtifactLinks(apiClient_t *apiClient, char *id);


// Advertised capabilities
//
// Returns the set of capability strings this aimee-kb instance supports. Phase 1 always returns [\"memory\", \"search\", \"index\"]. 
//
capabilities_response_t*
DefaultAPI_getCapabilities(apiClient_t *apiClient);


// Blast-radius computation for a file
//
blast_radius_response_t*
DefaultAPI_getCodeBlastRadius(apiClient_t *apiClient, char *project, char *file_path);


// Call sites for a symbol in the canonical code index
//
code_callers_response_t*
DefaultAPI_getCodeCallers(apiClient_t *apiClient, char *symbol, char *project, int *max_results);


// Symbol/identifier lookup across the canonical index
//
code_find_response_t*
DefaultAPI_getCodeFind(apiClient_t *apiClient, char *identifier, char *project, int *max_results);


// Project-level canonical code index counts and language breakdown
//
code_project_stats_response_t*
DefaultAPI_getCodeProjectStats(apiClient_t *apiClient, char *project);


// List projects in the canonical code index
//
code_projects_response_t*
DefaultAPI_getCodeProjects(apiClient_t *apiClient, int *max_results);


// Full-text code search across indexed file contents
//
code_search_response_t*
DefaultAPI_getCodeSearch(apiClient_t *apiClient, char *query, char *project, int *max_results);


// Definitions for a file in the canonical code index
//
code_structure_response_t*
DefaultAPI_getCodeStructure(apiClient_t *apiClient, char *project, char *file_path, int *max_results);


// Retrieve doc metadata by id
//
doc_metadata_response_t*
DefaultAPI_getDoc(apiClient_t *apiClient, long id);


// Canonical entity profile
//
entity_profile_response_t*
DefaultAPI_getEntityProfile(apiClient_t *apiClient, char *id);


// Service health check
//
// Returns {\"status\":\"ok\"} when the service is running.
//
health_response_t*
DefaultAPI_getHealth(apiClient_t *apiClient);


// Report background project ingest status
//
ingest_status_response_t*
DefaultAPI_getIngestStatus(apiClient_t *apiClient);


// Export fusion bandit decision data
//
object_t*
DefaultAPI_getIntelligenceBanditExport(apiClient_t *apiClient);


// Calibration readiness
//
object_t*
DefaultAPI_getIntelligenceCalibrationReadiness(apiClient_t *apiClient);


// Dry-run demotion readiness check
//
object_t*
DefaultAPI_getIntelligenceDemotionCheck(apiClient_t *apiClient);


// Report asynchronous knowledge ingest job status
//
job_status_response_t*
DefaultAPI_getJobStatus(apiClient_t *apiClient, long job_id);


// Report asynchronous knowledge ingest queue status
//
pipeline_status_response_t*
DefaultAPI_getPipelineStatus(apiClient_t *apiClient);


// List staged documents pending review
//
review_queue_response_t*
DefaultAPI_getReview(apiClient_t *apiClient, long cursor, int *limit);


// Service version
//
version_response_t*
DefaultAPI_getVersion(apiClient_t *apiClient);


// Report aimee-kb worker and background task status
//
workers_response_t*
DefaultAPI_getWorkers(apiClient_t *apiClient);


// Service health check (HEAD)
//
void
DefaultAPI_headHealth(apiClient_t *apiClient);


// Execute a versioned knowledge-service action
//
object_t*
DefaultAPI_postAction(apiClient_t *apiClient, char *action, object_t *body);


// Build a project knowledge index
//
code_build_response_t*
DefaultAPI_postCodeBuild(apiClient_t *apiClient, code_build_request_t *code_build_request);


// Request a canonical code index scan
//
code_scan_response_t*
DefaultAPI_postCodeScan(apiClient_t *apiClient, code_scan_request_t *code_scan_request);


// Incrementally update a project knowledge index
//
code_update_response_t*
DefaultAPI_postCodeUpdate(apiClient_t *apiClient, code_update_request_t *code_update_request);


// Upload a document for ingest
//
// Accepts multipart/form-data with a required `file` part and an optional `scope` field (default: global). Normalizes to markdown, stores in DB2, and queues an extract_doc job. Returns existing doc_id on idempotent re-upload. 
//
doc_ingest_response_t*
DefaultAPI_postDocs(apiClient_t *apiClient, binary_t* file, char *scope);


// Check which uploaded document hashes are absent
//
docs_manifest_response_t*
DefaultAPI_postDocsManifest(apiClient_t *apiClient, docs_manifest_request_t *docs_manifest_request);


// Drain the asynchronous knowledge ingest queue
//
drain_response_t*
DefaultAPI_postDrain(apiClient_t *apiClient, drain_request_t *drain_request);


// Find entities by name or context
//
entity_search_response_t*
DefaultAPI_postEntitySearch(apiClient_t *apiClient, entity_search_request_t *entity_search_request);


// Enqueue background project ingest
//
ingest_response_t*
DefaultAPI_postIngest(apiClient_t *apiClient, ingest_request_t *ingest_request);


// Close a sampled decision with its observed reward
//
object_t*
DefaultAPI_postIntelligenceBanditClose(apiClient_t *apiClient);


// Persist the production-default arm for a decision point
//
object_t*
DefaultAPI_postIntelligenceBanditPromote(apiClient_t *apiClient);


// Sample an arm for a decision point (server-side decision points)
//
object_t*
DefaultAPI_postIntelligenceBanditSample(apiClient_t *apiClient);


// Clear indexed knowledge for a project
//
maintenance_clear_response_t*
DefaultAPI_postMaintenanceClear(apiClient_t *apiClient, maintenance_clear_request_t *maintenance_clear_request);


// Reconcile orphaned vector records
//
maintenance_reconcile_response_t*
DefaultAPI_postMaintenanceReconcile(apiClient_t *apiClient, maintenance_reconcile_request_t *maintenance_reconcile_request);


// Repair a project knowledge index
//
maintenance_repair_response_t*
DefaultAPI_postMaintenanceRepair(apiClient_t *apiClient, maintenance_repair_request_t *maintenance_repair_request);


// Promote a release to active
//
void
DefaultAPI_postPromote(apiClient_t *apiClient, long id);


// Create a new corpus release
//
create_release_response_t*
DefaultAPI_postReleases(apiClient_t *apiClient, create_release_request_t *create_release_request);


// Accept a staged document
//
void
DefaultAPI_postReviewAccept(apiClient_t *apiClient, long id, review_accept_request_t *review_accept_request);


// Reject a staged document
//
void
DefaultAPI_postReviewReject(apiClient_t *apiClient, long id, review_reject_request_t *review_reject_request);


// Roll back to a prior release
//
void
DefaultAPI_postRollback(apiClient_t *apiClient, long id, rollback_request_t *rollback_request);


// Hybrid knowledge search
//
search_response_t*
DefaultAPI_postSearch(apiClient_t *apiClient, search_request_t *search_request);


