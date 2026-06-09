# DefaultApi

All URIs are relative to *http://127.0.0.1:8090/v1*

| Method | HTTP request | Description |
|------------- | ------------- | -------------|
| [**deleteDoc**](DefaultApi.md#deletedoc) | **DELETE** /docs/{id} | Delete a staged document |
| [**getActiveRelease**](DefaultApi.md#getactiverelease) | **GET** /releases/active | Get the currently active corpus release |
| [**getArtifact**](DefaultApi.md#getartifact) | **GET** /artifacts/{id} | Retrieve an artifact by UUID |
| [**getArtifactLinks**](DefaultApi.md#getartifactlinks) | **GET** /artifacts/{id}/links | Retrieve outgoing links from an artifact |
| [**getCapabilities**](DefaultApi.md#getcapabilities) | **GET** /capabilities | Advertised capabilities |
| [**getCodeBlastRadius**](DefaultApi.md#getcodeblastradius) | **GET** /code/blast-radius | Blast-radius computation for a file |
| [**getCodeCallers**](DefaultApi.md#getcodecallers) | **GET** /code/callers | Call sites for a symbol in the canonical code index |
| [**getCodeFind**](DefaultApi.md#getcodefind) | **GET** /code/find | Symbol/identifier lookup across the canonical index |
| [**getCodeProjectStats**](DefaultApi.md#getcodeprojectstats) | **GET** /code/project-stats | Project-level canonical code index counts and language breakdown |
| [**getCodeProjects**](DefaultApi.md#getcodeprojects) | **GET** /code/projects | List projects in the canonical code index |
| [**getCodeSearch**](DefaultApi.md#getcodesearch) | **GET** /code/search | Full-text code search across indexed file contents |
| [**getCodeStructure**](DefaultApi.md#getcodestructure) | **GET** /code/structure | Definitions for a file in the canonical code index |
| [**getDoc**](DefaultApi.md#getdoc) | **GET** /docs/{id} | Retrieve doc metadata by id |
| [**getEntityProfile**](DefaultApi.md#getentityprofile) | **GET** /entities/{id} | Canonical entity profile |
| [**getHealth**](DefaultApi.md#gethealth) | **GET** /health | Service health check |
| [**getIngestStatus**](DefaultApi.md#getingeststatus) | **GET** /ingest/status | Report background project ingest status |
| [**getIntelligenceBanditExport**](DefaultApi.md#getintelligencebanditexport) | **GET** /intelligence/bandit/export | Export fusion bandit decision data |
| [**getIntelligenceCalibrationReadiness**](DefaultApi.md#getintelligencecalibrationreadiness) | **GET** /intelligence/calibration/readiness | Calibration readiness |
| [**getIntelligenceDemotionCheck**](DefaultApi.md#getintelligencedemotioncheck) | **GET** /intelligence/demotion/check | Dry-run demotion readiness check |
| [**getJobStatus**](DefaultApi.md#getjobstatus) | **GET** /jobs/{job_id} | Report asynchronous knowledge ingest job status |
| [**getPipelineStatus**](DefaultApi.md#getpipelinestatus) | **GET** /pipeline/status | Report asynchronous knowledge ingest queue status |
| [**getReview**](DefaultApi.md#getreview) | **GET** /review | List staged documents pending review |
| [**getVersion**](DefaultApi.md#getversion) | **GET** /version | Service version |
| [**getWorkers**](DefaultApi.md#getworkers) | **GET** /workers | Report aimee-kb worker and background task status |
| [**headHealth**](DefaultApi.md#headhealth) | **HEAD** /health | Service health check (HEAD) |
| [**postAction**](DefaultApi.md#postaction) | **POST** /actions/{action} | Execute a versioned knowledge-service action |
| [**postCodeBuild**](DefaultApi.md#postcodebuild) | **POST** /code/build | Build a project knowledge index |
| [**postCodeScan**](DefaultApi.md#postcodescan) | **POST** /code/scan | Request a canonical code index scan |
| [**postCodeUpdate**](DefaultApi.md#postcodeupdate) | **POST** /code/update | Incrementally update a project knowledge index |
| [**postDocs**](DefaultApi.md#postdocs) | **POST** /docs | Upload a document for ingest |
| [**postDocsManifest**](DefaultApi.md#postdocsmanifest) | **POST** /docs/manifest | Check which uploaded document hashes are absent |
| [**postDrain**](DefaultApi.md#postdrain) | **POST** /drain | Drain the asynchronous knowledge ingest queue |
| [**postEntitySearch**](DefaultApi.md#postentitysearch) | **POST** /entities/search | Find entities by name or context |
| [**postIngest**](DefaultApi.md#postingest) | **POST** /ingest | Enqueue background project ingest |
| [**postIntelligenceBanditClose**](DefaultApi.md#postintelligencebanditclose) | **POST** /intelligence/bandit/close | Close a sampled decision with its observed reward |
| [**postIntelligenceBanditPromote**](DefaultApi.md#postintelligencebanditpromote) | **POST** /intelligence/bandit/promote | Persist the production-default arm for a decision point |
| [**postIntelligenceBanditSample**](DefaultApi.md#postintelligencebanditsample) | **POST** /intelligence/bandit/sample | Sample an arm for a decision point (server-side decision points) |
| [**postMaintenanceClear**](DefaultApi.md#postmaintenanceclear) | **POST** /maintenance/clear | Clear indexed knowledge for a project |
| [**postMaintenanceReconcile**](DefaultApi.md#postmaintenancereconcile) | **POST** /maintenance/reconcile | Reconcile orphaned vector records |
| [**postMaintenanceRepair**](DefaultApi.md#postmaintenancerepair) | **POST** /maintenance/repair | Repair a project knowledge index |
| [**postPromote**](DefaultApi.md#postpromote) | **POST** /releases/{id}/promote | Promote a release to active |
| [**postReleases**](DefaultApi.md#postreleases) | **POST** /releases | Create a new corpus release |
| [**postReviewAccept**](DefaultApi.md#postreviewaccept) | **POST** /review/{id}/accept | Accept a staged document |
| [**postReviewReject**](DefaultApi.md#postreviewreject) | **POST** /review/{id}/reject | Reject a staged document |
| [**postRollback**](DefaultApi.md#postrollback) | **POST** /releases/{id}/rollback | Roll back to a prior release |
| [**postSearch**](DefaultApi.md#postsearch) | **POST** /search | Hybrid knowledge search |



## deleteDoc

> deleteDoc(id)

Delete a staged document

### Example

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

### Parameters


| Name | Type | Description  | Notes |
|------------- | ------------- | ------------- | -------------|
| **id** | `number` |  | [Defaults to `undefined`] |

### Return type

`void` (Empty response body)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

- **Content-Type**: Not defined
- **Accept**: Not defined


### HTTP response details
| Status code | Description | Response headers |
|-------------|-------------|------------------|
| **200** | Document deleted |  -  |
| **401** | Unauthorized |  -  |
| **404** | Document not found |  -  |

[[Back to top]](#) [[Back to API list]](../README.md#api-endpoints) [[Back to Model list]](../README.md#models) [[Back to README]](../README.md)


## getActiveRelease

> ActiveReleaseResponse getActiveRelease()

Get the currently active corpus release

### Example

```ts
import {
  Configuration,
  DefaultApi,
} from '@aimee/kb-client';
import type { GetActiveReleaseRequest } from '@aimee/kb-client';

async function example() {
  console.log("🚀 Testing @aimee/kb-client SDK...");
  const config = new Configuration({ 
    // Configure HTTP bearer authorization: bearerAuth
    accessToken: "YOUR BEARER TOKEN",
  });
  const api = new DefaultApi(config);

  try {
    const data = await api.getActiveRelease();
    console.log(data);
  } catch (error) {
    console.error(error);
  }
}

// Run the test
example().catch(console.error);
```

### Parameters

This endpoint does not need any parameter.

### Return type

[**ActiveReleaseResponse**](ActiveReleaseResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

- **Content-Type**: Not defined
- **Accept**: `application/json`


### HTTP response details
| Status code | Description | Response headers |
|-------------|-------------|------------------|
| **200** | Active release (or null if none) |  -  |
| **401** | Unauthorized |  -  |

[[Back to top]](#) [[Back to API list]](../README.md#api-endpoints) [[Back to Model list]](../README.md#models) [[Back to README]](../README.md)


## getArtifact

> ArtifactResponse getArtifact(id)

Retrieve an artifact by UUID

### Example

```ts
import {
  Configuration,
  DefaultApi,
} from '@aimee/kb-client';
import type { GetArtifactRequest } from '@aimee/kb-client';

async function example() {
  console.log("🚀 Testing @aimee/kb-client SDK...");
  const config = new Configuration({ 
    // Configure HTTP bearer authorization: bearerAuth
    accessToken: "YOUR BEARER TOKEN",
  });
  const api = new DefaultApi(config);

  const body = {
    // string
    id: id_example,
  } satisfies GetArtifactRequest;

  try {
    const data = await api.getArtifact(body);
    console.log(data);
  } catch (error) {
    console.error(error);
  }
}

// Run the test
example().catch(console.error);
```

### Parameters


| Name | Type | Description  | Notes |
|------------- | ------------- | ------------- | -------------|
| **id** | `string` |  | [Defaults to `undefined`] |

### Return type

[**ArtifactResponse**](ArtifactResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

- **Content-Type**: Not defined
- **Accept**: `application/json`


### HTTP response details
| Status code | Description | Response headers |
|-------------|-------------|------------------|
| **200** | Artifact payload and citations |  -  |
| **401** | Unauthorized |  -  |
| **404** | Artifact not found |  -  |

[[Back to top]](#) [[Back to API list]](../README.md#api-endpoints) [[Back to Model list]](../README.md#models) [[Back to README]](../README.md)


## getArtifactLinks

> ArtifactLinksResponse getArtifactLinks(id)

Retrieve outgoing links from an artifact

### Example

```ts
import {
  Configuration,
  DefaultApi,
} from '@aimee/kb-client';
import type { GetArtifactLinksRequest } from '@aimee/kb-client';

async function example() {
  console.log("🚀 Testing @aimee/kb-client SDK...");
  const config = new Configuration({ 
    // Configure HTTP bearer authorization: bearerAuth
    accessToken: "YOUR BEARER TOKEN",
  });
  const api = new DefaultApi(config);

  const body = {
    // string
    id: id_example,
  } satisfies GetArtifactLinksRequest;

  try {
    const data = await api.getArtifactLinks(body);
    console.log(data);
  } catch (error) {
    console.error(error);
  }
}

// Run the test
example().catch(console.error);
```

### Parameters


| Name | Type | Description  | Notes |
|------------- | ------------- | ------------- | -------------|
| **id** | `string` |  | [Defaults to `undefined`] |

### Return type

[**ArtifactLinksResponse**](ArtifactLinksResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

- **Content-Type**: Not defined
- **Accept**: `application/json`


### HTTP response details
| Status code | Description | Response headers |
|-------------|-------------|------------------|
| **200** | Artifact links |  -  |
| **401** | Unauthorized |  -  |
| **404** | Artifact not found |  -  |

[[Back to top]](#) [[Back to API list]](../README.md#api-endpoints) [[Back to Model list]](../README.md#models) [[Back to README]](../README.md)


## getCapabilities

> CapabilitiesResponse getCapabilities()

Advertised capabilities

Returns the set of capability strings this aimee-kb instance supports. Phase 1 always returns [\&quot;memory\&quot;, \&quot;search\&quot;, \&quot;index\&quot;]. 

### Example

```ts
import {
  Configuration,
  DefaultApi,
} from '@aimee/kb-client';
import type { GetCapabilitiesRequest } from '@aimee/kb-client';

async function example() {
  console.log("🚀 Testing @aimee/kb-client SDK...");
  const config = new Configuration({ 
    // Configure HTTP bearer authorization: bearerAuth
    accessToken: "YOUR BEARER TOKEN",
  });
  const api = new DefaultApi(config);

  try {
    const data = await api.getCapabilities();
    console.log(data);
  } catch (error) {
    console.error(error);
  }
}

// Run the test
example().catch(console.error);
```

### Parameters

This endpoint does not need any parameter.

### Return type

[**CapabilitiesResponse**](CapabilitiesResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

- **Content-Type**: Not defined
- **Accept**: `application/json`


### HTTP response details
| Status code | Description | Response headers |
|-------------|-------------|------------------|
| **200** | Capability list |  -  |
| **401** | Unauthorized |  -  |

[[Back to top]](#) [[Back to API list]](../README.md#api-endpoints) [[Back to Model list]](../README.md#models) [[Back to README]](../README.md)


## getCodeBlastRadius

> BlastRadiusResponse getCodeBlastRadius(project, filePath)

Blast-radius computation for a file

### Example

```ts
import {
  Configuration,
  DefaultApi,
} from '@aimee/kb-client';
import type { GetCodeBlastRadiusRequest } from '@aimee/kb-client';

async function example() {
  console.log("🚀 Testing @aimee/kb-client SDK...");
  const config = new Configuration({ 
    // Configure HTTP bearer authorization: bearerAuth
    accessToken: "YOUR BEARER TOKEN",
  });
  const api = new DefaultApi(config);

  const body = {
    // string
    project: project_example,
    // string (optional)
    filePath: filePath_example,
  } satisfies GetCodeBlastRadiusRequest;

  try {
    const data = await api.getCodeBlastRadius(body);
    console.log(data);
  } catch (error) {
    console.error(error);
  }
}

// Run the test
example().catch(console.error);
```

### Parameters


| Name | Type | Description  | Notes |
|------------- | ------------- | ------------- | -------------|
| **project** | `string` |  | [Defaults to `undefined`] |
| **filePath** | `string` |  | [Optional] [Defaults to `undefined`] |

### Return type

[**BlastRadiusResponse**](BlastRadiusResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

- **Content-Type**: Not defined
- **Accept**: `application/json`


### HTTP response details
| Status code | Description | Response headers |
|-------------|-------------|------------------|
| **200** | Blast radius |  -  |
| **400** | Missing required parameters |  -  |
| **401** | Unauthorized |  -  |
| **404** | File not found in index |  -  |

[[Back to top]](#) [[Back to API list]](../README.md#api-endpoints) [[Back to Model list]](../README.md#models) [[Back to README]](../README.md)


## getCodeCallers

> CodeCallersResponse getCodeCallers(symbol, project, maxResults)

Call sites for a symbol in the canonical code index

### Example

```ts
import {
  Configuration,
  DefaultApi,
} from '@aimee/kb-client';
import type { GetCodeCallersRequest } from '@aimee/kb-client';

async function example() {
  console.log("🚀 Testing @aimee/kb-client SDK...");
  const config = new Configuration({ 
    // Configure HTTP bearer authorization: bearerAuth
    accessToken: "YOUR BEARER TOKEN",
  });
  const api = new DefaultApi(config);

  const body = {
    // string
    symbol: symbol_example,
    // string (optional)
    project: project_example,
    // number (optional)
    maxResults: 56,
  } satisfies GetCodeCallersRequest;

  try {
    const data = await api.getCodeCallers(body);
    console.log(data);
  } catch (error) {
    console.error(error);
  }
}

// Run the test
example().catch(console.error);
```

### Parameters


| Name | Type | Description  | Notes |
|------------- | ------------- | ------------- | -------------|
| **symbol** | `string` |  | [Defaults to `undefined`] |
| **project** | `string` |  | [Optional] [Defaults to `undefined`] |
| **maxResults** | `number` |  | [Optional] [Defaults to `20`] |

### Return type

[**CodeCallersResponse**](CodeCallersResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

- **Content-Type**: Not defined
- **Accept**: `application/json`


### HTTP response details
| Status code | Description | Response headers |
|-------------|-------------|------------------|
| **200** | Caller results |  -  |
| **400** | Missing symbol parameter |  -  |
| **401** | Unauthorized |  -  |
| **503** | Canonical index unavailable |  -  |

[[Back to top]](#) [[Back to API list]](../README.md#api-endpoints) [[Back to Model list]](../README.md#models) [[Back to README]](../README.md)


## getCodeFind

> CodeFindResponse getCodeFind(identifier, project, maxResults)

Symbol/identifier lookup across the canonical index

### Example

```ts
import {
  Configuration,
  DefaultApi,
} from '@aimee/kb-client';
import type { GetCodeFindRequest } from '@aimee/kb-client';

async function example() {
  console.log("🚀 Testing @aimee/kb-client SDK...");
  const config = new Configuration({ 
    // Configure HTTP bearer authorization: bearerAuth
    accessToken: "YOUR BEARER TOKEN",
  });
  const api = new DefaultApi(config);

  const body = {
    // string
    identifier: identifier_example,
    // string (optional)
    project: project_example,
    // number (optional)
    maxResults: 56,
  } satisfies GetCodeFindRequest;

  try {
    const data = await api.getCodeFind(body);
    console.log(data);
  } catch (error) {
    console.error(error);
  }
}

// Run the test
example().catch(console.error);
```

### Parameters


| Name | Type | Description  | Notes |
|------------- | ------------- | ------------- | -------------|
| **identifier** | `string` |  | [Defaults to `undefined`] |
| **project** | `string` |  | [Optional] [Defaults to `undefined`] |
| **maxResults** | `number` |  | [Optional] [Defaults to `20`] |

### Return type

[**CodeFindResponse**](CodeFindResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

- **Content-Type**: Not defined
- **Accept**: `application/json`


### HTTP response details
| Status code | Description | Response headers |
|-------------|-------------|------------------|
| **200** | Code find results |  -  |
| **400** | Missing identifier parameter |  -  |
| **401** | Unauthorized |  -  |

[[Back to top]](#) [[Back to API list]](../README.md#api-endpoints) [[Back to Model list]](../README.md#models) [[Back to README]](../README.md)


## getCodeProjectStats

> CodeProjectStatsResponse getCodeProjectStats(project)

Project-level canonical code index counts and language breakdown

### Example

```ts
import {
  Configuration,
  DefaultApi,
} from '@aimee/kb-client';
import type { GetCodeProjectStatsRequest } from '@aimee/kb-client';

async function example() {
  console.log("🚀 Testing @aimee/kb-client SDK...");
  const config = new Configuration({ 
    // Configure HTTP bearer authorization: bearerAuth
    accessToken: "YOUR BEARER TOKEN",
  });
  const api = new DefaultApi(config);

  const body = {
    // string
    project: project_example,
  } satisfies GetCodeProjectStatsRequest;

  try {
    const data = await api.getCodeProjectStats(body);
    console.log(data);
  } catch (error) {
    console.error(error);
  }
}

// Run the test
example().catch(console.error);
```

### Parameters


| Name | Type | Description  | Notes |
|------------- | ------------- | ------------- | -------------|
| **project** | `string` |  | [Defaults to `undefined`] |

### Return type

[**CodeProjectStatsResponse**](CodeProjectStatsResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

- **Content-Type**: Not defined
- **Accept**: `application/json`


### HTTP response details
| Status code | Description | Response headers |
|-------------|-------------|------------------|
| **200** | Project index statistics |  -  |
| **400** | Missing required parameters |  -  |
| **401** | Unauthorized |  -  |
| **503** | Canonical index unavailable |  -  |

[[Back to top]](#) [[Back to API list]](../README.md#api-endpoints) [[Back to Model list]](../README.md#models) [[Back to README]](../README.md)


## getCodeProjects

> CodeProjectsResponse getCodeProjects(maxResults)

List projects in the canonical code index

### Example

```ts
import {
  Configuration,
  DefaultApi,
} from '@aimee/kb-client';
import type { GetCodeProjectsRequest } from '@aimee/kb-client';

async function example() {
  console.log("🚀 Testing @aimee/kb-client SDK...");
  const config = new Configuration({ 
    // Configure HTTP bearer authorization: bearerAuth
    accessToken: "YOUR BEARER TOKEN",
  });
  const api = new DefaultApi(config);

  const body = {
    // number (optional)
    maxResults: 56,
  } satisfies GetCodeProjectsRequest;

  try {
    const data = await api.getCodeProjects(body);
    console.log(data);
  } catch (error) {
    console.error(error);
  }
}

// Run the test
example().catch(console.error);
```

### Parameters


| Name | Type | Description  | Notes |
|------------- | ------------- | ------------- | -------------|
| **maxResults** | `number` |  | [Optional] [Defaults to `100`] |

### Return type

[**CodeProjectsResponse**](CodeProjectsResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

- **Content-Type**: Not defined
- **Accept**: `application/json`


### HTTP response details
| Status code | Description | Response headers |
|-------------|-------------|------------------|
| **200** | Indexed projects |  -  |
| **401** | Unauthorized |  -  |
| **405** | Method not allowed |  -  |
| **503** | Canonical index unavailable |  -  |

[[Back to top]](#) [[Back to API list]](../README.md#api-endpoints) [[Back to Model list]](../README.md#models) [[Back to README]](../README.md)


## getCodeSearch

> CodeSearchResponse getCodeSearch(query, project, maxResults)

Full-text code search across indexed file contents

### Example

```ts
import {
  Configuration,
  DefaultApi,
} from '@aimee/kb-client';
import type { GetCodeSearchRequest } from '@aimee/kb-client';

async function example() {
  console.log("🚀 Testing @aimee/kb-client SDK...");
  const config = new Configuration({ 
    // Configure HTTP bearer authorization: bearerAuth
    accessToken: "YOUR BEARER TOKEN",
  });
  const api = new DefaultApi(config);

  const body = {
    // string
    query: query_example,
    // string (optional)
    project: project_example,
    // number (optional)
    maxResults: 56,
  } satisfies GetCodeSearchRequest;

  try {
    const data = await api.getCodeSearch(body);
    console.log(data);
  } catch (error) {
    console.error(error);
  }
}

// Run the test
example().catch(console.error);
```

### Parameters


| Name | Type | Description  | Notes |
|------------- | ------------- | ------------- | -------------|
| **query** | `string` |  | [Defaults to `undefined`] |
| **project** | `string` |  | [Optional] [Defaults to `undefined`] |
| **maxResults** | `number` |  | [Optional] [Defaults to `20`] |

### Return type

[**CodeSearchResponse**](CodeSearchResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

- **Content-Type**: Not defined
- **Accept**: `application/json`


### HTTP response details
| Status code | Description | Response headers |
|-------------|-------------|------------------|
| **200** | Code search results |  -  |
| **400** | Missing query parameter |  -  |
| **401** | Unauthorized |  -  |
| **503** | Canonical index unavailable |  -  |

[[Back to top]](#) [[Back to API list]](../README.md#api-endpoints) [[Back to Model list]](../README.md#models) [[Back to README]](../README.md)


## getCodeStructure

> CodeStructureResponse getCodeStructure(project, filePath, maxResults)

Definitions for a file in the canonical code index

### Example

```ts
import {
  Configuration,
  DefaultApi,
} from '@aimee/kb-client';
import type { GetCodeStructureRequest } from '@aimee/kb-client';

async function example() {
  console.log("🚀 Testing @aimee/kb-client SDK...");
  const config = new Configuration({ 
    // Configure HTTP bearer authorization: bearerAuth
    accessToken: "YOUR BEARER TOKEN",
  });
  const api = new DefaultApi(config);

  const body = {
    // string
    project: project_example,
    // string
    filePath: filePath_example,
    // number (optional)
    maxResults: 56,
  } satisfies GetCodeStructureRequest;

  try {
    const data = await api.getCodeStructure(body);
    console.log(data);
  } catch (error) {
    console.error(error);
  }
}

// Run the test
example().catch(console.error);
```

### Parameters


| Name | Type | Description  | Notes |
|------------- | ------------- | ------------- | -------------|
| **project** | `string` |  | [Defaults to `undefined`] |
| **filePath** | `string` |  | [Defaults to `undefined`] |
| **maxResults** | `number` |  | [Optional] [Defaults to `256`] |

### Return type

[**CodeStructureResponse**](CodeStructureResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

- **Content-Type**: Not defined
- **Accept**: `application/json`


### HTTP response details
| Status code | Description | Response headers |
|-------------|-------------|------------------|
| **200** | File definitions |  -  |
| **400** | Missing required parameters |  -  |
| **401** | Unauthorized |  -  |
| **503** | Canonical index unavailable |  -  |

[[Back to top]](#) [[Back to API list]](../README.md#api-endpoints) [[Back to Model list]](../README.md#models) [[Back to README]](../README.md)


## getDoc

> DocMetadataResponse getDoc(id)

Retrieve doc metadata by id

### Example

```ts
import {
  Configuration,
  DefaultApi,
} from '@aimee/kb-client';
import type { GetDocRequest } from '@aimee/kb-client';

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
  } satisfies GetDocRequest;

  try {
    const data = await api.getDoc(body);
    console.log(data);
  } catch (error) {
    console.error(error);
  }
}

// Run the test
example().catch(console.error);
```

### Parameters


| Name | Type | Description  | Notes |
|------------- | ------------- | ------------- | -------------|
| **id** | `number` |  | [Defaults to `undefined`] |

### Return type

[**DocMetadataResponse**](DocMetadataResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

- **Content-Type**: Not defined
- **Accept**: `application/json`


### HTTP response details
| Status code | Description | Response headers |
|-------------|-------------|------------------|
| **200** | Document metadata |  -  |
| **401** | Unauthorized |  -  |
| **404** | Document not found |  -  |

[[Back to top]](#) [[Back to API list]](../README.md#api-endpoints) [[Back to Model list]](../README.md#models) [[Back to README]](../README.md)


## getEntityProfile

> EntityProfileResponse getEntityProfile(id)

Canonical entity profile

### Example

```ts
import {
  Configuration,
  DefaultApi,
} from '@aimee/kb-client';
import type { GetEntityProfileRequest } from '@aimee/kb-client';

async function example() {
  console.log("🚀 Testing @aimee/kb-client SDK...");
  const config = new Configuration({ 
    // Configure HTTP bearer authorization: bearerAuth
    accessToken: "YOUR BEARER TOKEN",
  });
  const api = new DefaultApi(config);

  const body = {
    // string | Entity name (slug)
    id: id_example,
  } satisfies GetEntityProfileRequest;

  try {
    const data = await api.getEntityProfile(body);
    console.log(data);
  } catch (error) {
    console.error(error);
  }
}

// Run the test
example().catch(console.error);
```

### Parameters


| Name | Type | Description  | Notes |
|------------- | ------------- | ------------- | -------------|
| **id** | `string` | Entity name (slug) | [Defaults to `undefined`] |

### Return type

[**EntityProfileResponse**](EntityProfileResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

- **Content-Type**: Not defined
- **Accept**: `application/json`


### HTTP response details
| Status code | Description | Response headers |
|-------------|-------------|------------------|
| **200** | Entity profile |  -  |
| **401** | Unauthorized |  -  |
| **404** | Entity not found |  -  |

[[Back to top]](#) [[Back to API list]](../README.md#api-endpoints) [[Back to Model list]](../README.md#models) [[Back to README]](../README.md)


## getHealth

> HealthResponse getHealth()

Service health check

Returns {\&quot;status\&quot;:\&quot;ok\&quot;} when the service is running.

### Example

```ts
import {
  Configuration,
  DefaultApi,
} from '@aimee/kb-client';
import type { GetHealthRequest } from '@aimee/kb-client';

async function example() {
  console.log("🚀 Testing @aimee/kb-client SDK...");
  const api = new DefaultApi();

  try {
    const data = await api.getHealth();
    console.log(data);
  } catch (error) {
    console.error(error);
  }
}

// Run the test
example().catch(console.error);
```

### Parameters

This endpoint does not need any parameter.

### Return type

[**HealthResponse**](HealthResponse.md)

### Authorization

No authorization required

### HTTP request headers

- **Content-Type**: Not defined
- **Accept**: `application/json`


### HTTP response details
| Status code | Description | Response headers |
|-------------|-------------|------------------|
| **200** | Service is healthy |  -  |

[[Back to top]](#) [[Back to API list]](../README.md#api-endpoints) [[Back to Model list]](../README.md#models) [[Back to README]](../README.md)


## getIngestStatus

> IngestStatusResponse getIngestStatus()

Report background project ingest status

### Example

```ts
import {
  Configuration,
  DefaultApi,
} from '@aimee/kb-client';
import type { GetIngestStatusRequest } from '@aimee/kb-client';

async function example() {
  console.log("🚀 Testing @aimee/kb-client SDK...");
  const config = new Configuration({ 
    // Configure HTTP bearer authorization: bearerAuth
    accessToken: "YOUR BEARER TOKEN",
  });
  const api = new DefaultApi(config);

  try {
    const data = await api.getIngestStatus();
    console.log(data);
  } catch (error) {
    console.error(error);
  }
}

// Run the test
example().catch(console.error);
```

### Parameters

This endpoint does not need any parameter.

### Return type

[**IngestStatusResponse**](IngestStatusResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

- **Content-Type**: Not defined
- **Accept**: `application/json`


### HTTP response details
| Status code | Description | Response headers |
|-------------|-------------|------------------|
| **200** | Background ingest status |  -  |
| **401** | Unauthorized |  -  |
| **405** | Method not allowed |  -  |
| **503** | Ingest status unavailable |  -  |

[[Back to top]](#) [[Back to API list]](../README.md#api-endpoints) [[Back to Model list]](../README.md#models) [[Back to README]](../README.md)


## getIntelligenceBanditExport

> object getIntelligenceBanditExport()

Export fusion bandit decision data

### Example

```ts
import {
  Configuration,
  DefaultApi,
} from '@aimee/kb-client';
import type { GetIntelligenceBanditExportRequest } from '@aimee/kb-client';

async function example() {
  console.log("🚀 Testing @aimee/kb-client SDK...");
  const config = new Configuration({ 
    // Configure HTTP bearer authorization: bearerAuth
    accessToken: "YOUR BEARER TOKEN",
  });
  const api = new DefaultApi(config);

  try {
    const data = await api.getIntelligenceBanditExport();
    console.log(data);
  } catch (error) {
    console.error(error);
  }
}

// Run the test
example().catch(console.error);
```

### Parameters

This endpoint does not need any parameter.

### Return type

**object**

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

- **Content-Type**: Not defined
- **Accept**: `application/json`


### HTTP response details
| Status code | Description | Response headers |
|-------------|-------------|------------------|
| **200** | Bandit decisions and arm stats |  -  |
| **401** | Unauthorized |  -  |

[[Back to top]](#) [[Back to API list]](../README.md#api-endpoints) [[Back to Model list]](../README.md#models) [[Back to README]](../README.md)


## getIntelligenceCalibrationReadiness

> object getIntelligenceCalibrationReadiness()

Calibration readiness

### Example

```ts
import {
  Configuration,
  DefaultApi,
} from '@aimee/kb-client';
import type { GetIntelligenceCalibrationReadinessRequest } from '@aimee/kb-client';

async function example() {
  console.log("🚀 Testing @aimee/kb-client SDK...");
  const config = new Configuration({ 
    // Configure HTTP bearer authorization: bearerAuth
    accessToken: "YOUR BEARER TOKEN",
  });
  const api = new DefaultApi(config);

  try {
    const data = await api.getIntelligenceCalibrationReadiness();
    console.log(data);
  } catch (error) {
    console.error(error);
  }
}

// Run the test
example().catch(console.error);
```

### Parameters

This endpoint does not need any parameter.

### Return type

**object**

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

- **Content-Type**: Not defined
- **Accept**: `application/json`


### HTTP response details
| Status code | Description | Response headers |
|-------------|-------------|------------------|
| **200** | Calibration readiness summary |  -  |
| **401** | Unauthorized |  -  |

[[Back to top]](#) [[Back to API list]](../README.md#api-endpoints) [[Back to Model list]](../README.md#models) [[Back to README]](../README.md)


## getIntelligenceDemotionCheck

> object getIntelligenceDemotionCheck()

Dry-run demotion readiness check

### Example

```ts
import {
  Configuration,
  DefaultApi,
} from '@aimee/kb-client';
import type { GetIntelligenceDemotionCheckRequest } from '@aimee/kb-client';

async function example() {
  console.log("🚀 Testing @aimee/kb-client SDK...");
  const config = new Configuration({ 
    // Configure HTTP bearer authorization: bearerAuth
    accessToken: "YOUR BEARER TOKEN",
  });
  const api = new DefaultApi(config);

  try {
    const data = await api.getIntelligenceDemotionCheck();
    console.log(data);
  } catch (error) {
    console.error(error);
  }
}

// Run the test
example().catch(console.error);
```

### Parameters

This endpoint does not need any parameter.

### Return type

**object**

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

- **Content-Type**: Not defined
- **Accept**: `application/json`


### HTTP response details
| Status code | Description | Response headers |
|-------------|-------------|------------------|
| **200** | Demotion readiness summary |  -  |
| **401** | Unauthorized |  -  |

[[Back to top]](#) [[Back to API list]](../README.md#api-endpoints) [[Back to Model list]](../README.md#models) [[Back to README]](../README.md)


## getJobStatus

> JobStatusResponse getJobStatus(jobId)

Report asynchronous knowledge ingest job status

### Example

```ts
import {
  Configuration,
  DefaultApi,
} from '@aimee/kb-client';
import type { GetJobStatusRequest } from '@aimee/kb-client';

async function example() {
  console.log("🚀 Testing @aimee/kb-client SDK...");
  const config = new Configuration({ 
    // Configure HTTP bearer authorization: bearerAuth
    accessToken: "YOUR BEARER TOKEN",
  });
  const api = new DefaultApi(config);

  const body = {
    // number
    jobId: 789,
  } satisfies GetJobStatusRequest;

  try {
    const data = await api.getJobStatus(body);
    console.log(data);
  } catch (error) {
    console.error(error);
  }
}

// Run the test
example().catch(console.error);
```

### Parameters


| Name | Type | Description  | Notes |
|------------- | ------------- | ------------- | -------------|
| **jobId** | `number` |  | [Defaults to `undefined`] |

### Return type

[**JobStatusResponse**](JobStatusResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

- **Content-Type**: Not defined
- **Accept**: `application/json`


### HTTP response details
| Status code | Description | Response headers |
|-------------|-------------|------------------|
| **200** | Job status |  -  |
| **401** | Unauthorized |  -  |
| **404** | Job not found |  -  |
| **405** | Method not allowed |  -  |
| **503** | Job status unavailable |  -  |

[[Back to top]](#) [[Back to API list]](../README.md#api-endpoints) [[Back to Model list]](../README.md#models) [[Back to README]](../README.md)


## getPipelineStatus

> PipelineStatusResponse getPipelineStatus()

Report asynchronous knowledge ingest queue status

### Example

```ts
import {
  Configuration,
  DefaultApi,
} from '@aimee/kb-client';
import type { GetPipelineStatusRequest } from '@aimee/kb-client';

async function example() {
  console.log("🚀 Testing @aimee/kb-client SDK...");
  const config = new Configuration({ 
    // Configure HTTP bearer authorization: bearerAuth
    accessToken: "YOUR BEARER TOKEN",
  });
  const api = new DefaultApi(config);

  try {
    const data = await api.getPipelineStatus();
    console.log(data);
  } catch (error) {
    console.error(error);
  }
}

// Run the test
example().catch(console.error);
```

### Parameters

This endpoint does not need any parameter.

### Return type

[**PipelineStatusResponse**](PipelineStatusResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

- **Content-Type**: Not defined
- **Accept**: `application/json`


### HTTP response details
| Status code | Description | Response headers |
|-------------|-------------|------------------|
| **200** | Pipeline status |  -  |
| **401** | Unauthorized |  -  |
| **405** | Method not allowed |  -  |
| **503** | Queue unavailable |  -  |

[[Back to top]](#) [[Back to API list]](../README.md#api-endpoints) [[Back to Model list]](../README.md#models) [[Back to README]](../README.md)


## getReview

> ReviewQueueResponse getReview(cursor, limit)

List staged documents pending review

### Example

```ts
import {
  Configuration,
  DefaultApi,
} from '@aimee/kb-client';
import type { GetReviewRequest } from '@aimee/kb-client';

async function example() {
  console.log("🚀 Testing @aimee/kb-client SDK...");
  const config = new Configuration({ 
    // Configure HTTP bearer authorization: bearerAuth
    accessToken: "YOUR BEARER TOKEN",
  });
  const api = new DefaultApi(config);

  const body = {
    // number (optional)
    cursor: 789,
    // number (optional)
    limit: 56,
  } satisfies GetReviewRequest;

  try {
    const data = await api.getReview(body);
    console.log(data);
  } catch (error) {
    console.error(error);
  }
}

// Run the test
example().catch(console.error);
```

### Parameters


| Name | Type | Description  | Notes |
|------------- | ------------- | ------------- | -------------|
| **cursor** | `number` |  | [Optional] [Defaults to `undefined`] |
| **limit** | `number` |  | [Optional] [Defaults to `10`] |

### Return type

[**ReviewQueueResponse**](ReviewQueueResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

- **Content-Type**: Not defined
- **Accept**: `application/json`


### HTTP response details
| Status code | Description | Response headers |
|-------------|-------------|------------------|
| **200** | Review queue |  -  |
| **401** | Unauthorized |  -  |

[[Back to top]](#) [[Back to API list]](../README.md#api-endpoints) [[Back to Model list]](../README.md#models) [[Back to README]](../README.md)


## getVersion

> VersionResponse getVersion()

Service version

### Example

```ts
import {
  Configuration,
  DefaultApi,
} from '@aimee/kb-client';
import type { GetVersionRequest } from '@aimee/kb-client';

async function example() {
  console.log("🚀 Testing @aimee/kb-client SDK...");
  const config = new Configuration({ 
    // Configure HTTP bearer authorization: bearerAuth
    accessToken: "YOUR BEARER TOKEN",
  });
  const api = new DefaultApi(config);

  try {
    const data = await api.getVersion();
    console.log(data);
  } catch (error) {
    console.error(error);
  }
}

// Run the test
example().catch(console.error);
```

### Parameters

This endpoint does not need any parameter.

### Return type

[**VersionResponse**](VersionResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

- **Content-Type**: Not defined
- **Accept**: `application/json`


### HTTP response details
| Status code | Description | Response headers |
|-------------|-------------|------------------|
| **200** | Version information |  -  |
| **401** | Unauthorized |  -  |

[[Back to top]](#) [[Back to API list]](../README.md#api-endpoints) [[Back to Model list]](../README.md#models) [[Back to README]](../README.md)


## getWorkers

> WorkersResponse getWorkers()

Report aimee-kb worker and background task status

### Example

```ts
import {
  Configuration,
  DefaultApi,
} from '@aimee/kb-client';
import type { GetWorkersRequest } from '@aimee/kb-client';

async function example() {
  console.log("🚀 Testing @aimee/kb-client SDK...");
  const config = new Configuration({ 
    // Configure HTTP bearer authorization: bearerAuth
    accessToken: "YOUR BEARER TOKEN",
  });
  const api = new DefaultApi(config);

  try {
    const data = await api.getWorkers();
    console.log(data);
  } catch (error) {
    console.error(error);
  }
}

// Run the test
example().catch(console.error);
```

### Parameters

This endpoint does not need any parameter.

### Return type

[**WorkersResponse**](WorkersResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

- **Content-Type**: Not defined
- **Accept**: `application/json`


### HTTP response details
| Status code | Description | Response headers |
|-------------|-------------|------------------|
| **200** | Worker status |  -  |
| **401** | Unauthorized |  -  |
| **405** | Method not allowed |  -  |
| **503** | Workers unavailable |  -  |

[[Back to top]](#) [[Back to API list]](../README.md#api-endpoints) [[Back to Model list]](../README.md#models) [[Back to README]](../README.md)


## headHealth

> headHealth()

Service health check (HEAD)

### Example

```ts
import {
  Configuration,
  DefaultApi,
} from '@aimee/kb-client';
import type { HeadHealthRequest } from '@aimee/kb-client';

async function example() {
  console.log("🚀 Testing @aimee/kb-client SDK...");
  const api = new DefaultApi();

  try {
    const data = await api.headHealth();
    console.log(data);
  } catch (error) {
    console.error(error);
  }
}

// Run the test
example().catch(console.error);
```

### Parameters

This endpoint does not need any parameter.

### Return type

`void` (Empty response body)

### Authorization

No authorization required

### HTTP request headers

- **Content-Type**: Not defined
- **Accept**: Not defined


### HTTP response details
| Status code | Description | Response headers |
|-------------|-------------|------------------|
| **200** | Service is healthy |  -  |

[[Back to top]](#) [[Back to API list]](../README.md#api-endpoints) [[Back to Model list]](../README.md#models) [[Back to README]](../README.md)


## postAction

> object postAction(action, body)

Execute a versioned knowledge-service action

### Example

```ts
import {
  Configuration,
  DefaultApi,
} from '@aimee/kb-client';
import type { PostActionRequest } from '@aimee/kb-client';

async function example() {
  console.log("🚀 Testing @aimee/kb-client SDK...");
  const config = new Configuration({ 
    // Configure HTTP bearer authorization: bearerAuth
    accessToken: "YOUR BEARER TOKEN",
  });
  const api = new DefaultApi(config);

  const body = {
    // string
    action: action_example,
    // object (optional)
    body: Object,
  } satisfies PostActionRequest;

  try {
    const data = await api.postAction(body);
    console.log(data);
  } catch (error) {
    console.error(error);
  }
}

// Run the test
example().catch(console.error);
```

### Parameters


| Name | Type | Description  | Notes |
|------------- | ------------- | ------------- | -------------|
| **action** | `string` |  | [Defaults to `undefined`] |
| **body** | `object` |  | [Optional] |

### Return type

**object**

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

- **Content-Type**: `application/json`
- **Accept**: `application/json`


### HTTP response details
| Status code | Description | Response headers |
|-------------|-------------|------------------|
| **200** | Action response |  -  |
| **401** | Unauthorized |  -  |
| **404** | Unknown action |  -  |

[[Back to top]](#) [[Back to API list]](../README.md#api-endpoints) [[Back to Model list]](../README.md#models) [[Back to README]](../README.md)


## postCodeBuild

> CodeBuildResponse postCodeBuild(codeBuildRequest)

Build a project knowledge index

### Example

```ts
import {
  Configuration,
  DefaultApi,
} from '@aimee/kb-client';
import type { PostCodeBuildRequest } from '@aimee/kb-client';

async function example() {
  console.log("🚀 Testing @aimee/kb-client SDK...");
  const config = new Configuration({ 
    // Configure HTTP bearer authorization: bearerAuth
    accessToken: "YOUR BEARER TOKEN",
  });
  const api = new DefaultApi(config);

  const body = {
    // CodeBuildRequest
    codeBuildRequest: ...,
  } satisfies PostCodeBuildRequest;

  try {
    const data = await api.postCodeBuild(body);
    console.log(data);
  } catch (error) {
    console.error(error);
  }
}

// Run the test
example().catch(console.error);
```

### Parameters


| Name | Type | Description  | Notes |
|------------- | ------------- | ------------- | -------------|
| **codeBuildRequest** | [CodeBuildRequest](CodeBuildRequest.md) |  | |

### Return type

[**CodeBuildResponse**](CodeBuildResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

- **Content-Type**: `application/json`
- **Accept**: `application/json`


### HTTP response details
| Status code | Description | Response headers |
|-------------|-------------|------------------|
| **200** | Build complete |  -  |
| **400** | Missing required parameters |  -  |
| **401** | Unauthorized |  -  |
| **503** | Knowledge or vector store unavailable |  -  |

[[Back to top]](#) [[Back to API list]](../README.md#api-endpoints) [[Back to Model list]](../README.md#models) [[Back to README]](../README.md)


## postCodeScan

> CodeScanResponse postCodeScan(codeScanRequest)

Request a canonical code index scan

### Example

```ts
import {
  Configuration,
  DefaultApi,
} from '@aimee/kb-client';
import type { PostCodeScanRequest } from '@aimee/kb-client';

async function example() {
  console.log("🚀 Testing @aimee/kb-client SDK...");
  const config = new Configuration({ 
    // Configure HTTP bearer authorization: bearerAuth
    accessToken: "YOUR BEARER TOKEN",
  });
  const api = new DefaultApi(config);

  const body = {
    // CodeScanRequest
    codeScanRequest: ...,
  } satisfies PostCodeScanRequest;

  try {
    const data = await api.postCodeScan(body);
    console.log(data);
  } catch (error) {
    console.error(error);
  }
}

// Run the test
example().catch(console.error);
```

### Parameters


| Name | Type | Description  | Notes |
|------------- | ------------- | ------------- | -------------|
| **codeScanRequest** | [CodeScanRequest](CodeScanRequest.md) |  | |

### Return type

[**CodeScanResponse**](CodeScanResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

- **Content-Type**: `application/json`
- **Accept**: `application/json`


### HTTP response details
| Status code | Description | Response headers |
|-------------|-------------|------------------|
| **202** | Scan accepted |  -  |
| **401** | Unauthorized |  -  |
| **405** | Method not allowed |  -  |

[[Back to top]](#) [[Back to API list]](../README.md#api-endpoints) [[Back to Model list]](../README.md#models) [[Back to README]](../README.md)


## postCodeUpdate

> CodeUpdateResponse postCodeUpdate(codeUpdateRequest)

Incrementally update a project knowledge index

### Example

```ts
import {
  Configuration,
  DefaultApi,
} from '@aimee/kb-client';
import type { PostCodeUpdateRequest } from '@aimee/kb-client';

async function example() {
  console.log("🚀 Testing @aimee/kb-client SDK...");
  const config = new Configuration({ 
    // Configure HTTP bearer authorization: bearerAuth
    accessToken: "YOUR BEARER TOKEN",
  });
  const api = new DefaultApi(config);

  const body = {
    // CodeUpdateRequest
    codeUpdateRequest: ...,
  } satisfies PostCodeUpdateRequest;

  try {
    const data = await api.postCodeUpdate(body);
    console.log(data);
  } catch (error) {
    console.error(error);
  }
}

// Run the test
example().catch(console.error);
```

### Parameters


| Name | Type | Description  | Notes |
|------------- | ------------- | ------------- | -------------|
| **codeUpdateRequest** | [CodeUpdateRequest](CodeUpdateRequest.md) |  | |

### Return type

[**CodeUpdateResponse**](CodeUpdateResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

- **Content-Type**: `application/json`
- **Accept**: `application/json`


### HTTP response details
| Status code | Description | Response headers |
|-------------|-------------|------------------|
| **200** | Update complete |  -  |
| **400** | Missing required parameters |  -  |
| **401** | Unauthorized |  -  |
| **503** | Knowledge store unavailable |  -  |

[[Back to top]](#) [[Back to API list]](../README.md#api-endpoints) [[Back to Model list]](../README.md#models) [[Back to README]](../README.md)


## postDocs

> DocIngestResponse postDocs(file, scope)

Upload a document for ingest

Accepts multipart/form-data with a required &#x60;file&#x60; part and an optional &#x60;scope&#x60; field (default: global). Normalizes to markdown, stores in DB2, and queues an extract_doc job. Returns existing doc_id on idempotent re-upload. 

### Example

```ts
import {
  Configuration,
  DefaultApi,
} from '@aimee/kb-client';
import type { PostDocsRequest } from '@aimee/kb-client';

async function example() {
  console.log("🚀 Testing @aimee/kb-client SDK...");
  const config = new Configuration({ 
    // Configure HTTP bearer authorization: bearerAuth
    accessToken: "YOUR BEARER TOKEN",
  });
  const api = new DefaultApi(config);

  const body = {
    // Blob
    file: BINARY_DATA_HERE,
    // string (optional)
    scope: scope_example,
  } satisfies PostDocsRequest;

  try {
    const data = await api.postDocs(body);
    console.log(data);
  } catch (error) {
    console.error(error);
  }
}

// Run the test
example().catch(console.error);
```

### Parameters


| Name | Type | Description  | Notes |
|------------- | ------------- | ------------- | -------------|
| **file** | `Blob` |  | [Defaults to `undefined`] |
| **scope** | `string` |  | [Optional] [Defaults to `&#39;global&#39;`] |

### Return type

[**DocIngestResponse**](DocIngestResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

- **Content-Type**: `multipart/form-data`
- **Accept**: `application/json`


### HTTP response details
| Status code | Description | Response headers |
|-------------|-------------|------------------|
| **201** | Document ingested |  -  |
| **200** | Idempotent re-upload (existing doc_id returned) |  -  |
| **400** | Missing file part |  -  |
| **401** | Unauthorized |  -  |
| **422** | Normalization failed (converter error) |  -  |
| **503** | DB unavailable |  -  |

[[Back to top]](#) [[Back to API list]](../README.md#api-endpoints) [[Back to Model list]](../README.md#models) [[Back to README]](../README.md)


## postDocsManifest

> DocsManifestResponse postDocsManifest(docsManifestRequest)

Check which uploaded document hashes are absent

### Example

```ts
import {
  Configuration,
  DefaultApi,
} from '@aimee/kb-client';
import type { PostDocsManifestRequest } from '@aimee/kb-client';

async function example() {
  console.log("🚀 Testing @aimee/kb-client SDK...");
  const config = new Configuration({ 
    // Configure HTTP bearer authorization: bearerAuth
    accessToken: "YOUR BEARER TOKEN",
  });
  const api = new DefaultApi(config);

  const body = {
    // DocsManifestRequest
    docsManifestRequest: ...,
  } satisfies PostDocsManifestRequest;

  try {
    const data = await api.postDocsManifest(body);
    console.log(data);
  } catch (error) {
    console.error(error);
  }
}

// Run the test
example().catch(console.error);
```

### Parameters


| Name | Type | Description  | Notes |
|------------- | ------------- | ------------- | -------------|
| **docsManifestRequest** | [DocsManifestRequest](DocsManifestRequest.md) |  | |

### Return type

[**DocsManifestResponse**](DocsManifestResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

- **Content-Type**: `application/json`
- **Accept**: `application/json`


### HTTP response details
| Status code | Description | Response headers |
|-------------|-------------|------------------|
| **200** | Manifest diff |  -  |
| **400** | Invalid manifest request |  -  |
| **401** | Unauthorized |  -  |
| **405** | Method not allowed |  -  |
| **500** | Serialization failed |  -  |
| **503** | DB unavailable |  -  |

[[Back to top]](#) [[Back to API list]](../README.md#api-endpoints) [[Back to Model list]](../README.md#models) [[Back to README]](../README.md)


## postDrain

> DrainResponse postDrain(drainRequest)

Drain the asynchronous knowledge ingest queue

### Example

```ts
import {
  Configuration,
  DefaultApi,
} from '@aimee/kb-client';
import type { PostDrainRequest } from '@aimee/kb-client';

async function example() {
  console.log("🚀 Testing @aimee/kb-client SDK...");
  const config = new Configuration({ 
    // Configure HTTP bearer authorization: bearerAuth
    accessToken: "YOUR BEARER TOKEN",
  });
  const api = new DefaultApi(config);

  const body = {
    // DrainRequest (optional)
    drainRequest: ...,
  } satisfies PostDrainRequest;

  try {
    const data = await api.postDrain(body);
    console.log(data);
  } catch (error) {
    console.error(error);
  }
}

// Run the test
example().catch(console.error);
```

### Parameters


| Name | Type | Description  | Notes |
|------------- | ------------- | ------------- | -------------|
| **drainRequest** | [DrainRequest](DrainRequest.md) |  | [Optional] |

### Return type

[**DrainResponse**](DrainResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

- **Content-Type**: `application/json`
- **Accept**: `application/json`


### HTTP response details
| Status code | Description | Response headers |
|-------------|-------------|------------------|
| **200** | Queue drained |  -  |
| **401** | Unauthorized |  -  |
| **405** | Method not allowed |  -  |
| **500** | Queue drain failed |  -  |

[[Back to top]](#) [[Back to API list]](../README.md#api-endpoints) [[Back to Model list]](../README.md#models) [[Back to README]](../README.md)


## postEntitySearch

> EntitySearchResponse postEntitySearch(entitySearchRequest)

Find entities by name or context

### Example

```ts
import {
  Configuration,
  DefaultApi,
} from '@aimee/kb-client';
import type { PostEntitySearchRequest } from '@aimee/kb-client';

async function example() {
  console.log("🚀 Testing @aimee/kb-client SDK...");
  const config = new Configuration({ 
    // Configure HTTP bearer authorization: bearerAuth
    accessToken: "YOUR BEARER TOKEN",
  });
  const api = new DefaultApi(config);

  const body = {
    // EntitySearchRequest
    entitySearchRequest: ...,
  } satisfies PostEntitySearchRequest;

  try {
    const data = await api.postEntitySearch(body);
    console.log(data);
  } catch (error) {
    console.error(error);
  }
}

// Run the test
example().catch(console.error);
```

### Parameters


| Name | Type | Description  | Notes |
|------------- | ------------- | ------------- | -------------|
| **entitySearchRequest** | [EntitySearchRequest](EntitySearchRequest.md) |  | |

### Return type

[**EntitySearchResponse**](EntitySearchResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

- **Content-Type**: `application/json`
- **Accept**: `application/json`


### HTTP response details
| Status code | Description | Response headers |
|-------------|-------------|------------------|
| **200** | Entity search results |  -  |
| **400** | Missing query |  -  |
| **401** | Unauthorized |  -  |

[[Back to top]](#) [[Back to API list]](../README.md#api-endpoints) [[Back to Model list]](../README.md#models) [[Back to README]](../README.md)


## postIngest

> IngestResponse postIngest(ingestRequest)

Enqueue background project ingest

### Example

```ts
import {
  Configuration,
  DefaultApi,
} from '@aimee/kb-client';
import type { PostIngestRequest } from '@aimee/kb-client';

async function example() {
  console.log("🚀 Testing @aimee/kb-client SDK...");
  const config = new Configuration({ 
    // Configure HTTP bearer authorization: bearerAuth
    accessToken: "YOUR BEARER TOKEN",
  });
  const api = new DefaultApi(config);

  const body = {
    // IngestRequest (optional)
    ingestRequest: ...,
  } satisfies PostIngestRequest;

  try {
    const data = await api.postIngest(body);
    console.log(data);
  } catch (error) {
    console.error(error);
  }
}

// Run the test
example().catch(console.error);
```

### Parameters


| Name | Type | Description  | Notes |
|------------- | ------------- | ------------- | -------------|
| **ingestRequest** | [IngestRequest](IngestRequest.md) |  | [Optional] |

### Return type

[**IngestResponse**](IngestResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

- **Content-Type**: `application/json`
- **Accept**: `application/json`


### HTTP response details
| Status code | Description | Response headers |
|-------------|-------------|------------------|
| **202** | Ingest queued |  -  |
| **401** | Unauthorized |  -  |
| **405** | Method not allowed |  -  |
| **503** | Knowledge store unavailable |  -  |

[[Back to top]](#) [[Back to API list]](../README.md#api-endpoints) [[Back to Model list]](../README.md#models) [[Back to README]](../README.md)


## postIntelligenceBanditClose

> object postIntelligenceBanditClose()

Close a sampled decision with its observed reward

### Example

```ts
import {
  Configuration,
  DefaultApi,
} from '@aimee/kb-client';
import type { PostIntelligenceBanditCloseRequest } from '@aimee/kb-client';

async function example() {
  console.log("🚀 Testing @aimee/kb-client SDK...");
  const config = new Configuration({ 
    // Configure HTTP bearer authorization: bearerAuth
    accessToken: "YOUR BEARER TOKEN",
  });
  const api = new DefaultApi(config);

  try {
    const data = await api.postIntelligenceBanditClose();
    console.log(data);
  } catch (error) {
    console.error(error);
  }
}

// Run the test
example().catch(console.error);
```

### Parameters

This endpoint does not need any parameter.

### Return type

**object**

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

- **Content-Type**: Not defined
- **Accept**: `application/json`


### HTTP response details
| Status code | Description | Response headers |
|-------------|-------------|------------------|
| **200** | Close result |  -  |
| **401** | Unauthorized |  -  |

[[Back to top]](#) [[Back to API list]](../README.md#api-endpoints) [[Back to Model list]](../README.md#models) [[Back to README]](../README.md)


## postIntelligenceBanditPromote

> object postIntelligenceBanditPromote()

Persist the production-default arm for a decision point

### Example

```ts
import {
  Configuration,
  DefaultApi,
} from '@aimee/kb-client';
import type { PostIntelligenceBanditPromoteRequest } from '@aimee/kb-client';

async function example() {
  console.log("🚀 Testing @aimee/kb-client SDK...");
  const config = new Configuration({ 
    // Configure HTTP bearer authorization: bearerAuth
    accessToken: "YOUR BEARER TOKEN",
  });
  const api = new DefaultApi(config);

  try {
    const data = await api.postIntelligenceBanditPromote();
    console.log(data);
  } catch (error) {
    console.error(error);
  }
}

// Run the test
example().catch(console.error);
```

### Parameters

This endpoint does not need any parameter.

### Return type

**object**

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

- **Content-Type**: Not defined
- **Accept**: `application/json`


### HTTP response details
| Status code | Description | Response headers |
|-------------|-------------|------------------|
| **200** | Promotion result (rollback_arm) |  -  |
| **401** | Unauthorized |  -  |

[[Back to top]](#) [[Back to API list]](../README.md#api-endpoints) [[Back to Model list]](../README.md#models) [[Back to README]](../README.md)


## postIntelligenceBanditSample

> object postIntelligenceBanditSample()

Sample an arm for a decision point (server-side decision points)

### Example

```ts
import {
  Configuration,
  DefaultApi,
} from '@aimee/kb-client';
import type { PostIntelligenceBanditSampleRequest } from '@aimee/kb-client';

async function example() {
  console.log("🚀 Testing @aimee/kb-client SDK...");
  const config = new Configuration({ 
    // Configure HTTP bearer authorization: bearerAuth
    accessToken: "YOUR BEARER TOKEN",
  });
  const api = new DefaultApi(config);

  try {
    const data = await api.postIntelligenceBanditSample();
    console.log(data);
  } catch (error) {
    console.error(error);
  }
}

// Run the test
example().catch(console.error);
```

### Parameters

This endpoint does not need any parameter.

### Return type

**object**

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

- **Content-Type**: Not defined
- **Accept**: `application/json`


### HTTP response details
| Status code | Description | Response headers |
|-------------|-------------|------------------|
| **200** | Selected arm + decision id (or status disabled) |  -  |
| **401** | Unauthorized |  -  |

[[Back to top]](#) [[Back to API list]](../README.md#api-endpoints) [[Back to Model list]](../README.md#models) [[Back to README]](../README.md)


## postMaintenanceClear

> MaintenanceClearResponse postMaintenanceClear(maintenanceClearRequest)

Clear indexed knowledge for a project

### Example

```ts
import {
  Configuration,
  DefaultApi,
} from '@aimee/kb-client';
import type { PostMaintenanceClearRequest } from '@aimee/kb-client';

async function example() {
  console.log("🚀 Testing @aimee/kb-client SDK...");
  const config = new Configuration({ 
    // Configure HTTP bearer authorization: bearerAuth
    accessToken: "YOUR BEARER TOKEN",
  });
  const api = new DefaultApi(config);

  const body = {
    // MaintenanceClearRequest
    maintenanceClearRequest: ...,
  } satisfies PostMaintenanceClearRequest;

  try {
    const data = await api.postMaintenanceClear(body);
    console.log(data);
  } catch (error) {
    console.error(error);
  }
}

// Run the test
example().catch(console.error);
```

### Parameters


| Name | Type | Description  | Notes |
|------------- | ------------- | ------------- | -------------|
| **maintenanceClearRequest** | [MaintenanceClearRequest](MaintenanceClearRequest.md) |  | |

### Return type

[**MaintenanceClearResponse**](MaintenanceClearResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

- **Content-Type**: `application/json`
- **Accept**: `application/json`


### HTTP response details
| Status code | Description | Response headers |
|-------------|-------------|------------------|
| **200** | Project cleared |  -  |
| **400** | Missing project |  -  |
| **401** | Unauthorized |  -  |
| **500** | Clear failed |  -  |

[[Back to top]](#) [[Back to API list]](../README.md#api-endpoints) [[Back to Model list]](../README.md#models) [[Back to README]](../README.md)


## postMaintenanceReconcile

> MaintenanceReconcileResponse postMaintenanceReconcile(maintenanceReconcileRequest)

Reconcile orphaned vector records

### Example

```ts
import {
  Configuration,
  DefaultApi,
} from '@aimee/kb-client';
import type { PostMaintenanceReconcileRequest } from '@aimee/kb-client';

async function example() {
  console.log("🚀 Testing @aimee/kb-client SDK...");
  const config = new Configuration({ 
    // Configure HTTP bearer authorization: bearerAuth
    accessToken: "YOUR BEARER TOKEN",
  });
  const api = new DefaultApi(config);

  const body = {
    // MaintenanceReconcileRequest (optional)
    maintenanceReconcileRequest: ...,
  } satisfies PostMaintenanceReconcileRequest;

  try {
    const data = await api.postMaintenanceReconcile(body);
    console.log(data);
  } catch (error) {
    console.error(error);
  }
}

// Run the test
example().catch(console.error);
```

### Parameters


| Name | Type | Description  | Notes |
|------------- | ------------- | ------------- | -------------|
| **maintenanceReconcileRequest** | [MaintenanceReconcileRequest](MaintenanceReconcileRequest.md) |  | [Optional] |

### Return type

[**MaintenanceReconcileResponse**](MaintenanceReconcileResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

- **Content-Type**: `application/json`
- **Accept**: `application/json`


### HTTP response details
| Status code | Description | Response headers |
|-------------|-------------|------------------|
| **200** | Reconcile complete |  -  |
| **401** | Unauthorized |  -  |
| **500** | Reconcile failed |  -  |

[[Back to top]](#) [[Back to API list]](../README.md#api-endpoints) [[Back to Model list]](../README.md#models) [[Back to README]](../README.md)


## postMaintenanceRepair

> MaintenanceRepairResponse postMaintenanceRepair(maintenanceRepairRequest)

Repair a project knowledge index

### Example

```ts
import {
  Configuration,
  DefaultApi,
} from '@aimee/kb-client';
import type { PostMaintenanceRepairRequest } from '@aimee/kb-client';

async function example() {
  console.log("🚀 Testing @aimee/kb-client SDK...");
  const config = new Configuration({ 
    // Configure HTTP bearer authorization: bearerAuth
    accessToken: "YOUR BEARER TOKEN",
  });
  const api = new DefaultApi(config);

  const body = {
    // MaintenanceRepairRequest
    maintenanceRepairRequest: ...,
  } satisfies PostMaintenanceRepairRequest;

  try {
    const data = await api.postMaintenanceRepair(body);
    console.log(data);
  } catch (error) {
    console.error(error);
  }
}

// Run the test
example().catch(console.error);
```

### Parameters


| Name | Type | Description  | Notes |
|------------- | ------------- | ------------- | -------------|
| **maintenanceRepairRequest** | [MaintenanceRepairRequest](MaintenanceRepairRequest.md) |  | |

### Return type

[**MaintenanceRepairResponse**](MaintenanceRepairResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

- **Content-Type**: `application/json`
- **Accept**: `application/json`


### HTTP response details
| Status code | Description | Response headers |
|-------------|-------------|------------------|
| **200** | Repair complete |  -  |
| **400** | Missing required parameters |  -  |
| **401** | Unauthorized |  -  |
| **503** | Knowledge or vector store unavailable |  -  |

[[Back to top]](#) [[Back to API list]](../README.md#api-endpoints) [[Back to Model list]](../README.md#models) [[Back to README]](../README.md)


## postPromote

> postPromote(id)

Promote a release to active

### Example

```ts
import {
  Configuration,
  DefaultApi,
} from '@aimee/kb-client';
import type { PostPromoteRequest } from '@aimee/kb-client';

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
  } satisfies PostPromoteRequest;

  try {
    const data = await api.postPromote(body);
    console.log(data);
  } catch (error) {
    console.error(error);
  }
}

// Run the test
example().catch(console.error);
```

### Parameters


| Name | Type | Description  | Notes |
|------------- | ------------- | ------------- | -------------|
| **id** | `number` |  | [Defaults to `undefined`] |

### Return type

`void` (Empty response body)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

- **Content-Type**: Not defined
- **Accept**: Not defined


### HTTP response details
| Status code | Description | Response headers |
|-------------|-------------|------------------|
| **200** | Release promoted |  -  |
| **400** | Invalid id |  -  |
| **401** | Unauthorized |  -  |
| **409** | Promote failed |  -  |

[[Back to top]](#) [[Back to API list]](../README.md#api-endpoints) [[Back to Model list]](../README.md#models) [[Back to README]](../README.md)


## postReleases

> CreateReleaseResponse postReleases(createReleaseRequest)

Create a new corpus release

### Example

```ts
import {
  Configuration,
  DefaultApi,
} from '@aimee/kb-client';
import type { PostReleasesRequest } from '@aimee/kb-client';

async function example() {
  console.log("🚀 Testing @aimee/kb-client SDK...");
  const config = new Configuration({ 
    // Configure HTTP bearer authorization: bearerAuth
    accessToken: "YOUR BEARER TOKEN",
  });
  const api = new DefaultApi(config);

  const body = {
    // CreateReleaseRequest
    createReleaseRequest: ...,
  } satisfies PostReleasesRequest;

  try {
    const data = await api.postReleases(body);
    console.log(data);
  } catch (error) {
    console.error(error);
  }
}

// Run the test
example().catch(console.error);
```

### Parameters


| Name | Type | Description  | Notes |
|------------- | ------------- | ------------- | -------------|
| **createReleaseRequest** | [CreateReleaseRequest](CreateReleaseRequest.md) |  | |

### Return type

[**CreateReleaseResponse**](CreateReleaseResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

- **Content-Type**: `application/json`
- **Accept**: `application/json`


### HTTP response details
| Status code | Description | Response headers |
|-------------|-------------|------------------|
| **201** | Release created |  -  |
| **400** | Missing name |  -  |
| **401** | Unauthorized |  -  |
| **409** | Name already exists |  -  |

[[Back to top]](#) [[Back to API list]](../README.md#api-endpoints) [[Back to Model list]](../README.md#models) [[Back to README]](../README.md)


## postReviewAccept

> postReviewAccept(id, reviewAcceptRequest)

Accept a staged document

### Example

```ts
import {
  Configuration,
  DefaultApi,
} from '@aimee/kb-client';
import type { PostReviewAcceptRequest } from '@aimee/kb-client';

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
    // ReviewAcceptRequest (optional)
    reviewAcceptRequest: ...,
  } satisfies PostReviewAcceptRequest;

  try {
    const data = await api.postReviewAccept(body);
    console.log(data);
  } catch (error) {
    console.error(error);
  }
}

// Run the test
example().catch(console.error);
```

### Parameters


| Name | Type | Description  | Notes |
|------------- | ------------- | ------------- | -------------|
| **id** | `number` |  | [Defaults to `undefined`] |
| **reviewAcceptRequest** | [ReviewAcceptRequest](ReviewAcceptRequest.md) |  | [Optional] |

### Return type

`void` (Empty response body)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

- **Content-Type**: `application/json`
- **Accept**: Not defined


### HTTP response details
| Status code | Description | Response headers |
|-------------|-------------|------------------|
| **200** | Document accepted |  -  |
| **400** | Invalid id |  -  |
| **401** | Unauthorized |  -  |
| **404** | Document not found |  -  |

[[Back to top]](#) [[Back to API list]](../README.md#api-endpoints) [[Back to Model list]](../README.md#models) [[Back to README]](../README.md)


## postReviewReject

> postReviewReject(id, reviewRejectRequest)

Reject a staged document

### Example

```ts
import {
  Configuration,
  DefaultApi,
} from '@aimee/kb-client';
import type { PostReviewRejectRequest } from '@aimee/kb-client';

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
    // ReviewRejectRequest
    reviewRejectRequest: ...,
  } satisfies PostReviewRejectRequest;

  try {
    const data = await api.postReviewReject(body);
    console.log(data);
  } catch (error) {
    console.error(error);
  }
}

// Run the test
example().catch(console.error);
```

### Parameters


| Name | Type | Description  | Notes |
|------------- | ------------- | ------------- | -------------|
| **id** | `number` |  | [Defaults to `undefined`] |
| **reviewRejectRequest** | [ReviewRejectRequest](ReviewRejectRequest.md) |  | |

### Return type

`void` (Empty response body)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

- **Content-Type**: `application/json`
- **Accept**: Not defined


### HTTP response details
| Status code | Description | Response headers |
|-------------|-------------|------------------|
| **200** | Document rejected |  -  |
| **400** | Invalid id |  -  |
| **401** | Unauthorized |  -  |
| **404** | Document not found |  -  |

[[Back to top]](#) [[Back to API list]](../README.md#api-endpoints) [[Back to Model list]](../README.md#models) [[Back to README]](../README.md)


## postRollback

> postRollback(id, rollbackRequest)

Roll back to a prior release

### Example

```ts
import {
  Configuration,
  DefaultApi,
} from '@aimee/kb-client';
import type { PostRollbackRequest } from '@aimee/kb-client';

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
    // RollbackRequest (optional)
    rollbackRequest: ...,
  } satisfies PostRollbackRequest;

  try {
    const data = await api.postRollback(body);
    console.log(data);
  } catch (error) {
    console.error(error);
  }
}

// Run the test
example().catch(console.error);
```

### Parameters


| Name | Type | Description  | Notes |
|------------- | ------------- | ------------- | -------------|
| **id** | `number` |  | [Defaults to `undefined`] |
| **rollbackRequest** | [RollbackRequest](RollbackRequest.md) |  | [Optional] |

### Return type

`void` (Empty response body)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

- **Content-Type**: `application/json`
- **Accept**: Not defined


### HTTP response details
| Status code | Description | Response headers |
|-------------|-------------|------------------|
| **200** | Rolled back |  -  |
| **400** | Invalid id |  -  |
| **401** | Unauthorized |  -  |
| **409** | No prior release to roll back to |  -  |

[[Back to top]](#) [[Back to API list]](../README.md#api-endpoints) [[Back to Model list]](../README.md#models) [[Back to README]](../README.md)


## postSearch

> SearchResponse postSearch(searchRequest)

Hybrid knowledge search

### Example

```ts
import {
  Configuration,
  DefaultApi,
} from '@aimee/kb-client';
import type { PostSearchRequest } from '@aimee/kb-client';

async function example() {
  console.log("🚀 Testing @aimee/kb-client SDK...");
  const config = new Configuration({ 
    // Configure HTTP bearer authorization: bearerAuth
    accessToken: "YOUR BEARER TOKEN",
  });
  const api = new DefaultApi(config);

  const body = {
    // SearchRequest
    searchRequest: ...,
  } satisfies PostSearchRequest;

  try {
    const data = await api.postSearch(body);
    console.log(data);
  } catch (error) {
    console.error(error);
  }
}

// Run the test
example().catch(console.error);
```

### Parameters


| Name | Type | Description  | Notes |
|------------- | ------------- | ------------- | -------------|
| **searchRequest** | [SearchRequest](SearchRequest.md) |  | |

### Return type

[**SearchResponse**](SearchResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

- **Content-Type**: `application/json`
- **Accept**: `application/json`


### HTTP response details
| Status code | Description | Response headers |
|-------------|-------------|------------------|
| **200** | Search results |  -  |
| **400** | Bad request (missing query) |  -  |
| **401** | Unauthorized |  -  |

[[Back to top]](#) [[Back to API list]](../README.md#api-endpoints) [[Back to Model list]](../README.md#models) [[Back to README]](../README.md)

