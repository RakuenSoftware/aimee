# @aimee/kb-client@1.0.0

A TypeScript SDK client for the 127.0.0.1 API.

## Usage

First, install the SDK from npm.

```bash
npm install @aimee/kb-client --save
```

Next, try it out.


```ts
import {
  Configuration,
  DefaultApi,
} from '@aimee/kb-client';
import type { DeleteDocRequest } from '@aimee/kb-client';

async function example() {
  console.log("🚀 Testing @aimee/kb-client SDK...");
  const config = new Configuration({ 
    // Configure HTTP bearer authorization: bearerAuth
    accessToken: "YOUR BEARER TOKEN",
  });
  const api = new DefaultApi(config);

  const body = {
    // number
    id: 789,
  } satisfies DeleteDocRequest;

  try {
    const data = await api.deleteDoc(body);
    console.log(data);
  } catch (error) {
    console.error(error);
  }
}

// Run the test
example().catch(console.error);
```


## Documentation

### API Endpoints

All URIs are relative to *http://127.0.0.1:8090/v1*

| Class | Method | HTTP request | Description
| ----- | ------ | ------------ | -------------
*DefaultApi* | [**deleteDoc**](docs/DefaultApi.md#deletedoc) | **DELETE** /docs/{id} | Delete a staged document
*DefaultApi* | [**getActiveRelease**](docs/DefaultApi.md#getactiverelease) | **GET** /releases/active | Get the currently active corpus release
*DefaultApi* | [**getArtifact**](docs/DefaultApi.md#getartifact) | **GET** /artifacts/{id} | Retrieve an artifact by UUID
*DefaultApi* | [**getArtifactLinks**](docs/DefaultApi.md#getartifactlinks) | **GET** /artifacts/{id}/links | Retrieve outgoing links from an artifact
*DefaultApi* | [**getCapabilities**](docs/DefaultApi.md#getcapabilities) | **GET** /capabilities | Advertised capabilities
*DefaultApi* | [**getCodeBlastRadius**](docs/DefaultApi.md#getcodeblastradius) | **GET** /code/blast-radius | Blast-radius computation for a file
*DefaultApi* | [**getCodeCallers**](docs/DefaultApi.md#getcodecallers) | **GET** /code/callers | Call sites for a symbol in the canonical code index
*DefaultApi* | [**getCodeFind**](docs/DefaultApi.md#getcodefind) | **GET** /code/find | Symbol/identifier lookup across the canonical index
*DefaultApi* | [**getCodeProjectStats**](docs/DefaultApi.md#getcodeprojectstats) | **GET** /code/project-stats | Project-level canonical code index counts and language breakdown
*DefaultApi* | [**getCodeProjects**](docs/DefaultApi.md#getcodeprojects) | **GET** /code/projects | List projects in the canonical code index
*DefaultApi* | [**getCodeSearch**](docs/DefaultApi.md#getcodesearch) | **GET** /code/search | Full-text code search across indexed file contents
*DefaultApi* | [**getCodeStructure**](docs/DefaultApi.md#getcodestructure) | **GET** /code/structure | Definitions for a file in the canonical code index
*DefaultApi* | [**getDoc**](docs/DefaultApi.md#getdoc) | **GET** /docs/{id} | Retrieve doc metadata by id
*DefaultApi* | [**getEntityProfile**](docs/DefaultApi.md#getentityprofile) | **GET** /entities/{id} | Canonical entity profile
*DefaultApi* | [**getHealth**](docs/DefaultApi.md#gethealth) | **GET** /health | Service health check
*DefaultApi* | [**getIngestStatus**](docs/DefaultApi.md#getingeststatus) | **GET** /ingest/status | Report background project ingest status
*DefaultApi* | [**getIntelligenceBanditExport**](docs/DefaultApi.md#getintelligencebanditexport) | **GET** /intelligence/bandit/export | Export fusion bandit decision data
*DefaultApi* | [**getIntelligenceCalibrationReadiness**](docs/DefaultApi.md#getintelligencecalibrationreadiness) | **GET** /intelligence/calibration/readiness | Calibration readiness
*DefaultApi* | [**getIntelligenceDemotionCheck**](docs/DefaultApi.md#getintelligencedemotioncheck) | **GET** /intelligence/demotion/check | Dry-run demotion readiness check
*DefaultApi* | [**getJobStatus**](docs/DefaultApi.md#getjobstatus) | **GET** /jobs/{job_id} | Report asynchronous knowledge ingest job status
*DefaultApi* | [**getPipelineStatus**](docs/DefaultApi.md#getpipelinestatus) | **GET** /pipeline/status | Report asynchronous knowledge ingest queue status
*DefaultApi* | [**getReview**](docs/DefaultApi.md#getreview) | **GET** /review | List staged documents pending review
*DefaultApi* | [**getVersion**](docs/DefaultApi.md#getversion) | **GET** /version | Service version
*DefaultApi* | [**getWorkers**](docs/DefaultApi.md#getworkers) | **GET** /workers | Report aimee-kb worker and background task status
*DefaultApi* | [**headHealth**](docs/DefaultApi.md#headhealth) | **HEAD** /health | Service health check (HEAD)
*DefaultApi* | [**postAction**](docs/DefaultApi.md#postaction) | **POST** /actions/{action} | Execute a versioned knowledge-service action
*DefaultApi* | [**postCodeBuild**](docs/DefaultApi.md#postcodebuild) | **POST** /code/build | Build a project knowledge index
*DefaultApi* | [**postCodeScan**](docs/DefaultApi.md#postcodescan) | **POST** /code/scan | Request a canonical code index scan
*DefaultApi* | [**postCodeUpdate**](docs/DefaultApi.md#postcodeupdate) | **POST** /code/update | Incrementally update a project knowledge index
*DefaultApi* | [**postDocs**](docs/DefaultApi.md#postdocs) | **POST** /docs | Upload a document for ingest
*DefaultApi* | [**postDocsManifest**](docs/DefaultApi.md#postdocsmanifest) | **POST** /docs/manifest | Check which uploaded document hashes are absent
*DefaultApi* | [**postDrain**](docs/DefaultApi.md#postdrain) | **POST** /drain | Drain the asynchronous knowledge ingest queue
*DefaultApi* | [**postEntitySearch**](docs/DefaultApi.md#postentitysearch) | **POST** /entities/search | Find entities by name or context
*DefaultApi* | [**postIngest**](docs/DefaultApi.md#postingest) | **POST** /ingest | Enqueue background project ingest
*DefaultApi* | [**postIntelligenceBanditClose**](docs/DefaultApi.md#postintelligencebanditclose) | **POST** /intelligence/bandit/close | Close a sampled decision with its observed reward
*DefaultApi* | [**postIntelligenceBanditPromote**](docs/DefaultApi.md#postintelligencebanditpromote) | **POST** /intelligence/bandit/promote | Persist the production-default arm for a decision point
*DefaultApi* | [**postIntelligenceBanditSample**](docs/DefaultApi.md#postintelligencebanditsample) | **POST** /intelligence/bandit/sample | Sample an arm for a decision point (server-side decision points)
*DefaultApi* | [**postMaintenanceClear**](docs/DefaultApi.md#postmaintenanceclear) | **POST** /maintenance/clear | Clear indexed knowledge for a project
*DefaultApi* | [**postMaintenanceReconcile**](docs/DefaultApi.md#postmaintenancereconcile) | **POST** /maintenance/reconcile | Reconcile orphaned vector records
*DefaultApi* | [**postMaintenanceRepair**](docs/DefaultApi.md#postmaintenancerepair) | **POST** /maintenance/repair | Repair a project knowledge index
*DefaultApi* | [**postPromote**](docs/DefaultApi.md#postpromote) | **POST** /releases/{id}/promote | Promote a release to active
*DefaultApi* | [**postReleases**](docs/DefaultApi.md#postreleases) | **POST** /releases | Create a new corpus release
*DefaultApi* | [**postReviewAccept**](docs/DefaultApi.md#postreviewaccept) | **POST** /review/{id}/accept | Accept a staged document
*DefaultApi* | [**postReviewReject**](docs/DefaultApi.md#postreviewreject) | **POST** /review/{id}/reject | Reject a staged document
*DefaultApi* | [**postRollback**](docs/DefaultApi.md#postrollback) | **POST** /releases/{id}/rollback | Roll back to a prior release
*DefaultApi* | [**postSearch**](docs/DefaultApi.md#postsearch) | **POST** /search | Hybrid knowledge search


### Models

- [ActiveReleaseResponse](docs/ActiveReleaseResponse.md)
- [ArtifactLinksResponse](docs/ArtifactLinksResponse.md)
- [ArtifactLinksResponseLinksInner](docs/ArtifactLinksResponseLinksInner.md)
- [ArtifactResponse](docs/ArtifactResponse.md)
- [BlastRadiusResponse](docs/BlastRadiusResponse.md)
- [CapabilitiesResponse](docs/CapabilitiesResponse.md)
- [CodeBuildRequest](docs/CodeBuildRequest.md)
- [CodeBuildResponse](docs/CodeBuildResponse.md)
- [CodeCallerHit](docs/CodeCallerHit.md)
- [CodeCallersResponse](docs/CodeCallersResponse.md)
- [CodeDefinition](docs/CodeDefinition.md)
- [CodeFindHit](docs/CodeFindHit.md)
- [CodeFindResponse](docs/CodeFindResponse.md)
- [CodeProject](docs/CodeProject.md)
- [CodeProjectLanguage](docs/CodeProjectLanguage.md)
- [CodeProjectStatsResponse](docs/CodeProjectStatsResponse.md)
- [CodeProjectsResponse](docs/CodeProjectsResponse.md)
- [CodeScanRequest](docs/CodeScanRequest.md)
- [CodeScanResponse](docs/CodeScanResponse.md)
- [CodeSearchHit](docs/CodeSearchHit.md)
- [CodeSearchResponse](docs/CodeSearchResponse.md)
- [CodeStructureResponse](docs/CodeStructureResponse.md)
- [CodeUpdateRequest](docs/CodeUpdateRequest.md)
- [CodeUpdateResponse](docs/CodeUpdateResponse.md)
- [CreateReleaseRequest](docs/CreateReleaseRequest.md)
- [CreateReleaseResponse](docs/CreateReleaseResponse.md)
- [DocIngestResponse](docs/DocIngestResponse.md)
- [DocMetadataResponse](docs/DocMetadataResponse.md)
- [DocsManifestRequest](docs/DocsManifestRequest.md)
- [DocsManifestRequestDocsInner](docs/DocsManifestRequestDocsInner.md)
- [DocsManifestResponse](docs/DocsManifestResponse.md)
- [DocsManifestResponseMissingInner](docs/DocsManifestResponseMissingInner.md)
- [DrainRequest](docs/DrainRequest.md)
- [DrainResponse](docs/DrainResponse.md)
- [EntityProfileResponse](docs/EntityProfileResponse.md)
- [EntitySearchRequest](docs/EntitySearchRequest.md)
- [EntitySearchResponse](docs/EntitySearchResponse.md)
- [EntitySearchResponseEntitiesInner](docs/EntitySearchResponseEntitiesInner.md)
- [ErrorResponse](docs/ErrorResponse.md)
- [HealthResponse](docs/HealthResponse.md)
- [IngestRequest](docs/IngestRequest.md)
- [IngestResponse](docs/IngestResponse.md)
- [IngestStatusResponse](docs/IngestStatusResponse.md)
- [IngestStatusResponseQueue](docs/IngestStatusResponseQueue.md)
- [IngestStatusResponseRecentInner](docs/IngestStatusResponseRecentInner.md)
- [IngestStatusResponseWorkers](docs/IngestStatusResponseWorkers.md)
- [JobStatusResponse](docs/JobStatusResponse.md)
- [MaintenanceClearRequest](docs/MaintenanceClearRequest.md)
- [MaintenanceClearResponse](docs/MaintenanceClearResponse.md)
- [MaintenanceReconcileRequest](docs/MaintenanceReconcileRequest.md)
- [MaintenanceReconcileResponse](docs/MaintenanceReconcileResponse.md)
- [MaintenanceReconcileResponseMemory](docs/MaintenanceReconcileResponseMemory.md)
- [MaintenanceRepairRequest](docs/MaintenanceRepairRequest.md)
- [MaintenanceRepairResponse](docs/MaintenanceRepairResponse.md)
- [PipelineStatusResponse](docs/PipelineStatusResponse.md)
- [PipelineStatusResponseQueue](docs/PipelineStatusResponseQueue.md)
- [ReviewAcceptRequest](docs/ReviewAcceptRequest.md)
- [ReviewQueueResponse](docs/ReviewQueueResponse.md)
- [ReviewRejectRequest](docs/ReviewRejectRequest.md)
- [RollbackRequest](docs/RollbackRequest.md)
- [SearchHit](docs/SearchHit.md)
- [SearchHitCitationsInner](docs/SearchHitCitationsInner.md)
- [SearchRequest](docs/SearchRequest.md)
- [SearchResponse](docs/SearchResponse.md)
- [VersionResponse](docs/VersionResponse.md)
- [WorkersResponse](docs/WorkersResponse.md)

### Authorization


Authentication schemes defined for the API:
<a id="bearerAuth"></a>
#### bearerAuth


- **Type**: HTTP Bearer Token authentication

## About

This TypeScript SDK client supports the [Fetch API](https://fetch.spec.whatwg.org/)
and is automatically generated by the
[OpenAPI Generator](https://openapi-generator.tech) project:

- API version: `1`
- Package version: `1.0.0`
- Generator version: `7.22.0`
- Build package: `org.openapitools.codegen.languages.TypeScriptFetchClientCodegen`

The generated npm module supports the following:

- Environments
  * Node.js
  * Webpack
  * Browserify
- Language levels
  * ES5 - you must have a Promises/A+ library installed
  * ES6
- Module systems
  * CommonJS
  * ES6 module system


## Development

### Building

To build the TypeScript source code, you need to have Node.js and npm installed.
After cloning the repository, navigate to the project directory and run:

```bash
npm install
npm run build
```

### Publishing

Once you've built the package, you can publish it to npm:

```bash
npm publish
```

## License

[]()
