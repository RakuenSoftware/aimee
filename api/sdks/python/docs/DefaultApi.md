# aimee_kb.DefaultApi

All URIs are relative to *http://127.0.0.1:8090/v1*

Method | HTTP request | Description
------------- | ------------- | -------------
[**delete_doc**](DefaultApi.md#delete_doc) | **DELETE** /docs/{id} | Delete a staged document
[**get_active_release**](DefaultApi.md#get_active_release) | **GET** /releases/active | Get the currently active corpus release
[**get_artifact**](DefaultApi.md#get_artifact) | **GET** /artifacts/{id} | Retrieve an artifact by UUID
[**get_artifact_links**](DefaultApi.md#get_artifact_links) | **GET** /artifacts/{id}/links | Retrieve outgoing links from an artifact
[**get_capabilities**](DefaultApi.md#get_capabilities) | **GET** /capabilities | Advertised capabilities
[**get_code_blast_radius**](DefaultApi.md#get_code_blast_radius) | **GET** /code/blast-radius | Blast-radius computation for a file
[**get_code_callers**](DefaultApi.md#get_code_callers) | **GET** /code/callers | Call sites for a symbol in the canonical code index
[**get_code_find**](DefaultApi.md#get_code_find) | **GET** /code/find | Symbol/identifier lookup across the canonical index
[**get_code_project_stats**](DefaultApi.md#get_code_project_stats) | **GET** /code/project-stats | Project-level canonical code index counts and language breakdown
[**get_code_projects**](DefaultApi.md#get_code_projects) | **GET** /code/projects | List projects in the canonical code index
[**get_code_search**](DefaultApi.md#get_code_search) | **GET** /code/search | Full-text code search across indexed file contents
[**get_code_structure**](DefaultApi.md#get_code_structure) | **GET** /code/structure | Definitions for a file in the canonical code index
[**get_doc**](DefaultApi.md#get_doc) | **GET** /docs/{id} | Retrieve doc metadata by id
[**get_entity_profile**](DefaultApi.md#get_entity_profile) | **GET** /entities/{id} | Canonical entity profile
[**get_health**](DefaultApi.md#get_health) | **GET** /health | Service health check
[**get_ingest_status**](DefaultApi.md#get_ingest_status) | **GET** /ingest/status | Report background project ingest status
[**get_intelligence_bandit_export**](DefaultApi.md#get_intelligence_bandit_export) | **GET** /intelligence/bandit/export | Export fusion bandit decision data
[**get_intelligence_calibration_readiness**](DefaultApi.md#get_intelligence_calibration_readiness) | **GET** /intelligence/calibration/readiness | Calibration readiness
[**get_intelligence_demotion_check**](DefaultApi.md#get_intelligence_demotion_check) | **GET** /intelligence/demotion/check | Dry-run demotion readiness check
[**get_job_status**](DefaultApi.md#get_job_status) | **GET** /jobs/{job_id} | Report asynchronous knowledge ingest job status
[**get_pipeline_status**](DefaultApi.md#get_pipeline_status) | **GET** /pipeline/status | Report asynchronous knowledge ingest queue status
[**get_review**](DefaultApi.md#get_review) | **GET** /review | List staged documents pending review
[**get_version**](DefaultApi.md#get_version) | **GET** /version | Service version
[**get_workers**](DefaultApi.md#get_workers) | **GET** /workers | Report aimee-kb worker and background task status
[**head_health**](DefaultApi.md#head_health) | **HEAD** /health | Service health check (HEAD)
[**post_action**](DefaultApi.md#post_action) | **POST** /actions/{action} | Execute a versioned knowledge-service action
[**post_code_build**](DefaultApi.md#post_code_build) | **POST** /code/build | Build a project knowledge index
[**post_code_scan**](DefaultApi.md#post_code_scan) | **POST** /code/scan | Request a canonical code index scan
[**post_code_update**](DefaultApi.md#post_code_update) | **POST** /code/update | Incrementally update a project knowledge index
[**post_docs**](DefaultApi.md#post_docs) | **POST** /docs | Upload a document for ingest
[**post_docs_manifest**](DefaultApi.md#post_docs_manifest) | **POST** /docs/manifest | Check which uploaded document hashes are absent
[**post_drain**](DefaultApi.md#post_drain) | **POST** /drain | Drain the asynchronous knowledge ingest queue
[**post_entity_search**](DefaultApi.md#post_entity_search) | **POST** /entities/search | Find entities by name or context
[**post_ingest**](DefaultApi.md#post_ingest) | **POST** /ingest | Enqueue background project ingest
[**post_intelligence_bandit_close**](DefaultApi.md#post_intelligence_bandit_close) | **POST** /intelligence/bandit/close | Close a sampled decision with its observed reward
[**post_intelligence_bandit_promote**](DefaultApi.md#post_intelligence_bandit_promote) | **POST** /intelligence/bandit/promote | Persist the production-default arm for a decision point
[**post_intelligence_bandit_sample**](DefaultApi.md#post_intelligence_bandit_sample) | **POST** /intelligence/bandit/sample | Sample an arm for a decision point (server-side decision points)
[**post_maintenance_clear**](DefaultApi.md#post_maintenance_clear) | **POST** /maintenance/clear | Clear indexed knowledge for a project
[**post_maintenance_reconcile**](DefaultApi.md#post_maintenance_reconcile) | **POST** /maintenance/reconcile | Reconcile orphaned vector records
[**post_maintenance_repair**](DefaultApi.md#post_maintenance_repair) | **POST** /maintenance/repair | Repair a project knowledge index
[**post_promote**](DefaultApi.md#post_promote) | **POST** /releases/{id}/promote | Promote a release to active
[**post_releases**](DefaultApi.md#post_releases) | **POST** /releases | Create a new corpus release
[**post_review_accept**](DefaultApi.md#post_review_accept) | **POST** /review/{id}/accept | Accept a staged document
[**post_review_reject**](DefaultApi.md#post_review_reject) | **POST** /review/{id}/reject | Reject a staged document
[**post_rollback**](DefaultApi.md#post_rollback) | **POST** /releases/{id}/rollback | Roll back to a prior release
[**post_search**](DefaultApi.md#post_search) | **POST** /search | Hybrid knowledge search


# **delete_doc**
> delete_doc(id)

Delete a staged document

### Example

* Bearer Authentication (bearerAuth):

```python
import aimee_kb
from aimee_kb.rest import ApiException
from pprint import pprint

# Defining the host is optional and defaults to http://127.0.0.1:8090/v1
# See configuration.py for a list of all supported configuration parameters.
configuration = aimee_kb.Configuration(
    host = "http://127.0.0.1:8090/v1"
)

# The client must configure the authentication and authorization parameters
# in accordance with the API server security policy.
# Examples for each auth method are provided below, use the example that
# satisfies your auth use case.

# Configure Bearer authorization: bearerAuth
configuration = aimee_kb.Configuration(
    access_token = os.environ["BEARER_TOKEN"]
)

# Enter a context with an instance of the API client
with aimee_kb.ApiClient(configuration) as api_client:
    # Create an instance of the API class
    api_instance = aimee_kb.DefaultApi(api_client)
    id = 56 # int | 

    try:
        # Delete a staged document
        api_instance.delete_doc(id)
    except Exception as e:
        print("Exception when calling DefaultApi->delete_doc: %s\n" % e)
```



### Parameters


Name | Type | Description  | Notes
------------- | ------------- | ------------- | -------------
 **id** | **int**|  | 

### Return type

void (empty response body)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

 - **Content-Type**: Not defined
 - **Accept**: Not defined

### HTTP response details

| Status code | Description | Response headers |
|-------------|-------------|------------------|
**200** | Document deleted |  -  |
**401** | Unauthorized |  -  |
**404** | Document not found |  -  |

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)

# **get_active_release**
> ActiveReleaseResponse get_active_release()

Get the currently active corpus release

### Example

* Bearer Authentication (bearerAuth):

```python
import aimee_kb
from aimee_kb.models.active_release_response import ActiveReleaseResponse
from aimee_kb.rest import ApiException
from pprint import pprint

# Defining the host is optional and defaults to http://127.0.0.1:8090/v1
# See configuration.py for a list of all supported configuration parameters.
configuration = aimee_kb.Configuration(
    host = "http://127.0.0.1:8090/v1"
)

# The client must configure the authentication and authorization parameters
# in accordance with the API server security policy.
# Examples for each auth method are provided below, use the example that
# satisfies your auth use case.

# Configure Bearer authorization: bearerAuth
configuration = aimee_kb.Configuration(
    access_token = os.environ["BEARER_TOKEN"]
)

# Enter a context with an instance of the API client
with aimee_kb.ApiClient(configuration) as api_client:
    # Create an instance of the API class
    api_instance = aimee_kb.DefaultApi(api_client)

    try:
        # Get the currently active corpus release
        api_response = api_instance.get_active_release()
        print("The response of DefaultApi->get_active_release:\n")
        pprint(api_response)
    except Exception as e:
        print("Exception when calling DefaultApi->get_active_release: %s\n" % e)
```



### Parameters

This endpoint does not need any parameter.

### Return type

[**ActiveReleaseResponse**](ActiveReleaseResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

 - **Content-Type**: Not defined
 - **Accept**: application/json

### HTTP response details

| Status code | Description | Response headers |
|-------------|-------------|------------------|
**200** | Active release (or null if none) |  -  |
**401** | Unauthorized |  -  |

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)

# **get_artifact**
> ArtifactResponse get_artifact(id)

Retrieve an artifact by UUID

### Example

* Bearer Authentication (bearerAuth):

```python
import aimee_kb
from aimee_kb.models.artifact_response import ArtifactResponse
from aimee_kb.rest import ApiException
from pprint import pprint

# Defining the host is optional and defaults to http://127.0.0.1:8090/v1
# See configuration.py for a list of all supported configuration parameters.
configuration = aimee_kb.Configuration(
    host = "http://127.0.0.1:8090/v1"
)

# The client must configure the authentication and authorization parameters
# in accordance with the API server security policy.
# Examples for each auth method are provided below, use the example that
# satisfies your auth use case.

# Configure Bearer authorization: bearerAuth
configuration = aimee_kb.Configuration(
    access_token = os.environ["BEARER_TOKEN"]
)

# Enter a context with an instance of the API client
with aimee_kb.ApiClient(configuration) as api_client:
    # Create an instance of the API class
    api_instance = aimee_kb.DefaultApi(api_client)
    id = 'id_example' # str | 

    try:
        # Retrieve an artifact by UUID
        api_response = api_instance.get_artifact(id)
        print("The response of DefaultApi->get_artifact:\n")
        pprint(api_response)
    except Exception as e:
        print("Exception when calling DefaultApi->get_artifact: %s\n" % e)
```



### Parameters


Name | Type | Description  | Notes
------------- | ------------- | ------------- | -------------
 **id** | **str**|  | 

### Return type

[**ArtifactResponse**](ArtifactResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

 - **Content-Type**: Not defined
 - **Accept**: application/json

### HTTP response details

| Status code | Description | Response headers |
|-------------|-------------|------------------|
**200** | Artifact payload and citations |  -  |
**401** | Unauthorized |  -  |
**404** | Artifact not found |  -  |

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)

# **get_artifact_links**
> ArtifactLinksResponse get_artifact_links(id)

Retrieve outgoing links from an artifact

### Example

* Bearer Authentication (bearerAuth):

```python
import aimee_kb
from aimee_kb.models.artifact_links_response import ArtifactLinksResponse
from aimee_kb.rest import ApiException
from pprint import pprint

# Defining the host is optional and defaults to http://127.0.0.1:8090/v1
# See configuration.py for a list of all supported configuration parameters.
configuration = aimee_kb.Configuration(
    host = "http://127.0.0.1:8090/v1"
)

# The client must configure the authentication and authorization parameters
# in accordance with the API server security policy.
# Examples for each auth method are provided below, use the example that
# satisfies your auth use case.

# Configure Bearer authorization: bearerAuth
configuration = aimee_kb.Configuration(
    access_token = os.environ["BEARER_TOKEN"]
)

# Enter a context with an instance of the API client
with aimee_kb.ApiClient(configuration) as api_client:
    # Create an instance of the API class
    api_instance = aimee_kb.DefaultApi(api_client)
    id = 'id_example' # str | 

    try:
        # Retrieve outgoing links from an artifact
        api_response = api_instance.get_artifact_links(id)
        print("The response of DefaultApi->get_artifact_links:\n")
        pprint(api_response)
    except Exception as e:
        print("Exception when calling DefaultApi->get_artifact_links: %s\n" % e)
```



### Parameters


Name | Type | Description  | Notes
------------- | ------------- | ------------- | -------------
 **id** | **str**|  | 

### Return type

[**ArtifactLinksResponse**](ArtifactLinksResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

 - **Content-Type**: Not defined
 - **Accept**: application/json

### HTTP response details

| Status code | Description | Response headers |
|-------------|-------------|------------------|
**200** | Artifact links |  -  |
**401** | Unauthorized |  -  |
**404** | Artifact not found |  -  |

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)

# **get_capabilities**
> CapabilitiesResponse get_capabilities()

Advertised capabilities

Returns the set of capability strings this aimee-kb instance supports.
Phase 1 always returns ["memory", "search", "index"].


### Example

* Bearer Authentication (bearerAuth):

```python
import aimee_kb
from aimee_kb.models.capabilities_response import CapabilitiesResponse
from aimee_kb.rest import ApiException
from pprint import pprint

# Defining the host is optional and defaults to http://127.0.0.1:8090/v1
# See configuration.py for a list of all supported configuration parameters.
configuration = aimee_kb.Configuration(
    host = "http://127.0.0.1:8090/v1"
)

# The client must configure the authentication and authorization parameters
# in accordance with the API server security policy.
# Examples for each auth method are provided below, use the example that
# satisfies your auth use case.

# Configure Bearer authorization: bearerAuth
configuration = aimee_kb.Configuration(
    access_token = os.environ["BEARER_TOKEN"]
)

# Enter a context with an instance of the API client
with aimee_kb.ApiClient(configuration) as api_client:
    # Create an instance of the API class
    api_instance = aimee_kb.DefaultApi(api_client)

    try:
        # Advertised capabilities
        api_response = api_instance.get_capabilities()
        print("The response of DefaultApi->get_capabilities:\n")
        pprint(api_response)
    except Exception as e:
        print("Exception when calling DefaultApi->get_capabilities: %s\n" % e)
```



### Parameters

This endpoint does not need any parameter.

### Return type

[**CapabilitiesResponse**](CapabilitiesResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

 - **Content-Type**: Not defined
 - **Accept**: application/json

### HTTP response details

| Status code | Description | Response headers |
|-------------|-------------|------------------|
**200** | Capability list |  -  |
**401** | Unauthorized |  -  |

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)

# **get_code_blast_radius**
> BlastRadiusResponse get_code_blast_radius(project, file_path=file_path)

Blast-radius computation for a file

### Example

* Bearer Authentication (bearerAuth):

```python
import aimee_kb
from aimee_kb.models.blast_radius_response import BlastRadiusResponse
from aimee_kb.rest import ApiException
from pprint import pprint

# Defining the host is optional and defaults to http://127.0.0.1:8090/v1
# See configuration.py for a list of all supported configuration parameters.
configuration = aimee_kb.Configuration(
    host = "http://127.0.0.1:8090/v1"
)

# The client must configure the authentication and authorization parameters
# in accordance with the API server security policy.
# Examples for each auth method are provided below, use the example that
# satisfies your auth use case.

# Configure Bearer authorization: bearerAuth
configuration = aimee_kb.Configuration(
    access_token = os.environ["BEARER_TOKEN"]
)

# Enter a context with an instance of the API client
with aimee_kb.ApiClient(configuration) as api_client:
    # Create an instance of the API class
    api_instance = aimee_kb.DefaultApi(api_client)
    project = 'project_example' # str | 
    file_path = 'file_path_example' # str |  (optional)

    try:
        # Blast-radius computation for a file
        api_response = api_instance.get_code_blast_radius(project, file_path=file_path)
        print("The response of DefaultApi->get_code_blast_radius:\n")
        pprint(api_response)
    except Exception as e:
        print("Exception when calling DefaultApi->get_code_blast_radius: %s\n" % e)
```



### Parameters


Name | Type | Description  | Notes
------------- | ------------- | ------------- | -------------
 **project** | **str**|  | 
 **file_path** | **str**|  | [optional] 

### Return type

[**BlastRadiusResponse**](BlastRadiusResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

 - **Content-Type**: Not defined
 - **Accept**: application/json

### HTTP response details

| Status code | Description | Response headers |
|-------------|-------------|------------------|
**200** | Blast radius |  -  |
**400** | Missing required parameters |  -  |
**401** | Unauthorized |  -  |
**404** | File not found in index |  -  |

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)

# **get_code_callers**
> CodeCallersResponse get_code_callers(symbol, project=project, max_results=max_results)

Call sites for a symbol in the canonical code index

### Example

* Bearer Authentication (bearerAuth):

```python
import aimee_kb
from aimee_kb.models.code_callers_response import CodeCallersResponse
from aimee_kb.rest import ApiException
from pprint import pprint

# Defining the host is optional and defaults to http://127.0.0.1:8090/v1
# See configuration.py for a list of all supported configuration parameters.
configuration = aimee_kb.Configuration(
    host = "http://127.0.0.1:8090/v1"
)

# The client must configure the authentication and authorization parameters
# in accordance with the API server security policy.
# Examples for each auth method are provided below, use the example that
# satisfies your auth use case.

# Configure Bearer authorization: bearerAuth
configuration = aimee_kb.Configuration(
    access_token = os.environ["BEARER_TOKEN"]
)

# Enter a context with an instance of the API client
with aimee_kb.ApiClient(configuration) as api_client:
    # Create an instance of the API class
    api_instance = aimee_kb.DefaultApi(api_client)
    symbol = 'symbol_example' # str | 
    project = 'project_example' # str |  (optional)
    max_results = 20 # int |  (optional) (default to 20)

    try:
        # Call sites for a symbol in the canonical code index
        api_response = api_instance.get_code_callers(symbol, project=project, max_results=max_results)
        print("The response of DefaultApi->get_code_callers:\n")
        pprint(api_response)
    except Exception as e:
        print("Exception when calling DefaultApi->get_code_callers: %s\n" % e)
```



### Parameters


Name | Type | Description  | Notes
------------- | ------------- | ------------- | -------------
 **symbol** | **str**|  | 
 **project** | **str**|  | [optional] 
 **max_results** | **int**|  | [optional] [default to 20]

### Return type

[**CodeCallersResponse**](CodeCallersResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

 - **Content-Type**: Not defined
 - **Accept**: application/json

### HTTP response details

| Status code | Description | Response headers |
|-------------|-------------|------------------|
**200** | Caller results |  -  |
**400** | Missing symbol parameter |  -  |
**401** | Unauthorized |  -  |
**503** | Canonical index unavailable |  -  |

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)

# **get_code_find**
> CodeFindResponse get_code_find(identifier, project=project, max_results=max_results)

Symbol/identifier lookup across the canonical index

### Example

* Bearer Authentication (bearerAuth):

```python
import aimee_kb
from aimee_kb.models.code_find_response import CodeFindResponse
from aimee_kb.rest import ApiException
from pprint import pprint

# Defining the host is optional and defaults to http://127.0.0.1:8090/v1
# See configuration.py for a list of all supported configuration parameters.
configuration = aimee_kb.Configuration(
    host = "http://127.0.0.1:8090/v1"
)

# The client must configure the authentication and authorization parameters
# in accordance with the API server security policy.
# Examples for each auth method are provided below, use the example that
# satisfies your auth use case.

# Configure Bearer authorization: bearerAuth
configuration = aimee_kb.Configuration(
    access_token = os.environ["BEARER_TOKEN"]
)

# Enter a context with an instance of the API client
with aimee_kb.ApiClient(configuration) as api_client:
    # Create an instance of the API class
    api_instance = aimee_kb.DefaultApi(api_client)
    identifier = 'identifier_example' # str | 
    project = 'project_example' # str |  (optional)
    max_results = 20 # int |  (optional) (default to 20)

    try:
        # Symbol/identifier lookup across the canonical index
        api_response = api_instance.get_code_find(identifier, project=project, max_results=max_results)
        print("The response of DefaultApi->get_code_find:\n")
        pprint(api_response)
    except Exception as e:
        print("Exception when calling DefaultApi->get_code_find: %s\n" % e)
```



### Parameters


Name | Type | Description  | Notes
------------- | ------------- | ------------- | -------------
 **identifier** | **str**|  | 
 **project** | **str**|  | [optional] 
 **max_results** | **int**|  | [optional] [default to 20]

### Return type

[**CodeFindResponse**](CodeFindResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

 - **Content-Type**: Not defined
 - **Accept**: application/json

### HTTP response details

| Status code | Description | Response headers |
|-------------|-------------|------------------|
**200** | Code find results |  -  |
**400** | Missing identifier parameter |  -  |
**401** | Unauthorized |  -  |

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)

# **get_code_project_stats**
> CodeProjectStatsResponse get_code_project_stats(project)

Project-level canonical code index counts and language breakdown

### Example

* Bearer Authentication (bearerAuth):

```python
import aimee_kb
from aimee_kb.models.code_project_stats_response import CodeProjectStatsResponse
from aimee_kb.rest import ApiException
from pprint import pprint

# Defining the host is optional and defaults to http://127.0.0.1:8090/v1
# See configuration.py for a list of all supported configuration parameters.
configuration = aimee_kb.Configuration(
    host = "http://127.0.0.1:8090/v1"
)

# The client must configure the authentication and authorization parameters
# in accordance with the API server security policy.
# Examples for each auth method are provided below, use the example that
# satisfies your auth use case.

# Configure Bearer authorization: bearerAuth
configuration = aimee_kb.Configuration(
    access_token = os.environ["BEARER_TOKEN"]
)

# Enter a context with an instance of the API client
with aimee_kb.ApiClient(configuration) as api_client:
    # Create an instance of the API class
    api_instance = aimee_kb.DefaultApi(api_client)
    project = 'project_example' # str | 

    try:
        # Project-level canonical code index counts and language breakdown
        api_response = api_instance.get_code_project_stats(project)
        print("The response of DefaultApi->get_code_project_stats:\n")
        pprint(api_response)
    except Exception as e:
        print("Exception when calling DefaultApi->get_code_project_stats: %s\n" % e)
```



### Parameters


Name | Type | Description  | Notes
------------- | ------------- | ------------- | -------------
 **project** | **str**|  | 

### Return type

[**CodeProjectStatsResponse**](CodeProjectStatsResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

 - **Content-Type**: Not defined
 - **Accept**: application/json

### HTTP response details

| Status code | Description | Response headers |
|-------------|-------------|------------------|
**200** | Project index statistics |  -  |
**400** | Missing required parameters |  -  |
**401** | Unauthorized |  -  |
**503** | Canonical index unavailable |  -  |

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)

# **get_code_projects**
> CodeProjectsResponse get_code_projects(max_results=max_results)

List projects in the canonical code index

### Example

* Bearer Authentication (bearerAuth):

```python
import aimee_kb
from aimee_kb.models.code_projects_response import CodeProjectsResponse
from aimee_kb.rest import ApiException
from pprint import pprint

# Defining the host is optional and defaults to http://127.0.0.1:8090/v1
# See configuration.py for a list of all supported configuration parameters.
configuration = aimee_kb.Configuration(
    host = "http://127.0.0.1:8090/v1"
)

# The client must configure the authentication and authorization parameters
# in accordance with the API server security policy.
# Examples for each auth method are provided below, use the example that
# satisfies your auth use case.

# Configure Bearer authorization: bearerAuth
configuration = aimee_kb.Configuration(
    access_token = os.environ["BEARER_TOKEN"]
)

# Enter a context with an instance of the API client
with aimee_kb.ApiClient(configuration) as api_client:
    # Create an instance of the API class
    api_instance = aimee_kb.DefaultApi(api_client)
    max_results = 100 # int |  (optional) (default to 100)

    try:
        # List projects in the canonical code index
        api_response = api_instance.get_code_projects(max_results=max_results)
        print("The response of DefaultApi->get_code_projects:\n")
        pprint(api_response)
    except Exception as e:
        print("Exception when calling DefaultApi->get_code_projects: %s\n" % e)
```



### Parameters


Name | Type | Description  | Notes
------------- | ------------- | ------------- | -------------
 **max_results** | **int**|  | [optional] [default to 100]

### Return type

[**CodeProjectsResponse**](CodeProjectsResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

 - **Content-Type**: Not defined
 - **Accept**: application/json

### HTTP response details

| Status code | Description | Response headers |
|-------------|-------------|------------------|
**200** | Indexed projects |  -  |
**401** | Unauthorized |  -  |
**405** | Method not allowed |  -  |
**503** | Canonical index unavailable |  -  |

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)

# **get_code_search**
> CodeSearchResponse get_code_search(query, project=project, max_results=max_results)

Full-text code search across indexed file contents

### Example

* Bearer Authentication (bearerAuth):

```python
import aimee_kb
from aimee_kb.models.code_search_response import CodeSearchResponse
from aimee_kb.rest import ApiException
from pprint import pprint

# Defining the host is optional and defaults to http://127.0.0.1:8090/v1
# See configuration.py for a list of all supported configuration parameters.
configuration = aimee_kb.Configuration(
    host = "http://127.0.0.1:8090/v1"
)

# The client must configure the authentication and authorization parameters
# in accordance with the API server security policy.
# Examples for each auth method are provided below, use the example that
# satisfies your auth use case.

# Configure Bearer authorization: bearerAuth
configuration = aimee_kb.Configuration(
    access_token = os.environ["BEARER_TOKEN"]
)

# Enter a context with an instance of the API client
with aimee_kb.ApiClient(configuration) as api_client:
    # Create an instance of the API class
    api_instance = aimee_kb.DefaultApi(api_client)
    query = 'query_example' # str | 
    project = 'project_example' # str |  (optional)
    max_results = 20 # int |  (optional) (default to 20)

    try:
        # Full-text code search across indexed file contents
        api_response = api_instance.get_code_search(query, project=project, max_results=max_results)
        print("The response of DefaultApi->get_code_search:\n")
        pprint(api_response)
    except Exception as e:
        print("Exception when calling DefaultApi->get_code_search: %s\n" % e)
```



### Parameters


Name | Type | Description  | Notes
------------- | ------------- | ------------- | -------------
 **query** | **str**|  | 
 **project** | **str**|  | [optional] 
 **max_results** | **int**|  | [optional] [default to 20]

### Return type

[**CodeSearchResponse**](CodeSearchResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

 - **Content-Type**: Not defined
 - **Accept**: application/json

### HTTP response details

| Status code | Description | Response headers |
|-------------|-------------|------------------|
**200** | Code search results |  -  |
**400** | Missing query parameter |  -  |
**401** | Unauthorized |  -  |
**503** | Canonical index unavailable |  -  |

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)

# **get_code_structure**
> CodeStructureResponse get_code_structure(project, file_path, max_results=max_results)

Definitions for a file in the canonical code index

### Example

* Bearer Authentication (bearerAuth):

```python
import aimee_kb
from aimee_kb.models.code_structure_response import CodeStructureResponse
from aimee_kb.rest import ApiException
from pprint import pprint

# Defining the host is optional and defaults to http://127.0.0.1:8090/v1
# See configuration.py for a list of all supported configuration parameters.
configuration = aimee_kb.Configuration(
    host = "http://127.0.0.1:8090/v1"
)

# The client must configure the authentication and authorization parameters
# in accordance with the API server security policy.
# Examples for each auth method are provided below, use the example that
# satisfies your auth use case.

# Configure Bearer authorization: bearerAuth
configuration = aimee_kb.Configuration(
    access_token = os.environ["BEARER_TOKEN"]
)

# Enter a context with an instance of the API client
with aimee_kb.ApiClient(configuration) as api_client:
    # Create an instance of the API class
    api_instance = aimee_kb.DefaultApi(api_client)
    project = 'project_example' # str | 
    file_path = 'file_path_example' # str | 
    max_results = 256 # int |  (optional) (default to 256)

    try:
        # Definitions for a file in the canonical code index
        api_response = api_instance.get_code_structure(project, file_path, max_results=max_results)
        print("The response of DefaultApi->get_code_structure:\n")
        pprint(api_response)
    except Exception as e:
        print("Exception when calling DefaultApi->get_code_structure: %s\n" % e)
```



### Parameters


Name | Type | Description  | Notes
------------- | ------------- | ------------- | -------------
 **project** | **str**|  | 
 **file_path** | **str**|  | 
 **max_results** | **int**|  | [optional] [default to 256]

### Return type

[**CodeStructureResponse**](CodeStructureResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

 - **Content-Type**: Not defined
 - **Accept**: application/json

### HTTP response details

| Status code | Description | Response headers |
|-------------|-------------|------------------|
**200** | File definitions |  -  |
**400** | Missing required parameters |  -  |
**401** | Unauthorized |  -  |
**503** | Canonical index unavailable |  -  |

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)

# **get_doc**
> DocMetadataResponse get_doc(id)

Retrieve doc metadata by id

### Example

* Bearer Authentication (bearerAuth):

```python
import aimee_kb
from aimee_kb.models.doc_metadata_response import DocMetadataResponse
from aimee_kb.rest import ApiException
from pprint import pprint

# Defining the host is optional and defaults to http://127.0.0.1:8090/v1
# See configuration.py for a list of all supported configuration parameters.
configuration = aimee_kb.Configuration(
    host = "http://127.0.0.1:8090/v1"
)

# The client must configure the authentication and authorization parameters
# in accordance with the API server security policy.
# Examples for each auth method are provided below, use the example that
# satisfies your auth use case.

# Configure Bearer authorization: bearerAuth
configuration = aimee_kb.Configuration(
    access_token = os.environ["BEARER_TOKEN"]
)

# Enter a context with an instance of the API client
with aimee_kb.ApiClient(configuration) as api_client:
    # Create an instance of the API class
    api_instance = aimee_kb.DefaultApi(api_client)
    id = 56 # int | 

    try:
        # Retrieve doc metadata by id
        api_response = api_instance.get_doc(id)
        print("The response of DefaultApi->get_doc:\n")
        pprint(api_response)
    except Exception as e:
        print("Exception when calling DefaultApi->get_doc: %s\n" % e)
```



### Parameters


Name | Type | Description  | Notes
------------- | ------------- | ------------- | -------------
 **id** | **int**|  | 

### Return type

[**DocMetadataResponse**](DocMetadataResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

 - **Content-Type**: Not defined
 - **Accept**: application/json

### HTTP response details

| Status code | Description | Response headers |
|-------------|-------------|------------------|
**200** | Document metadata |  -  |
**401** | Unauthorized |  -  |
**404** | Document not found |  -  |

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)

# **get_entity_profile**
> EntityProfileResponse get_entity_profile(id)

Canonical entity profile

### Example

* Bearer Authentication (bearerAuth):

```python
import aimee_kb
from aimee_kb.models.entity_profile_response import EntityProfileResponse
from aimee_kb.rest import ApiException
from pprint import pprint

# Defining the host is optional and defaults to http://127.0.0.1:8090/v1
# See configuration.py for a list of all supported configuration parameters.
configuration = aimee_kb.Configuration(
    host = "http://127.0.0.1:8090/v1"
)

# The client must configure the authentication and authorization parameters
# in accordance with the API server security policy.
# Examples for each auth method are provided below, use the example that
# satisfies your auth use case.

# Configure Bearer authorization: bearerAuth
configuration = aimee_kb.Configuration(
    access_token = os.environ["BEARER_TOKEN"]
)

# Enter a context with an instance of the API client
with aimee_kb.ApiClient(configuration) as api_client:
    # Create an instance of the API class
    api_instance = aimee_kb.DefaultApi(api_client)
    id = 'id_example' # str | Entity name (slug)

    try:
        # Canonical entity profile
        api_response = api_instance.get_entity_profile(id)
        print("The response of DefaultApi->get_entity_profile:\n")
        pprint(api_response)
    except Exception as e:
        print("Exception when calling DefaultApi->get_entity_profile: %s\n" % e)
```



### Parameters


Name | Type | Description  | Notes
------------- | ------------- | ------------- | -------------
 **id** | **str**| Entity name (slug) | 

### Return type

[**EntityProfileResponse**](EntityProfileResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

 - **Content-Type**: Not defined
 - **Accept**: application/json

### HTTP response details

| Status code | Description | Response headers |
|-------------|-------------|------------------|
**200** | Entity profile |  -  |
**401** | Unauthorized |  -  |
**404** | Entity not found |  -  |

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)

# **get_health**
> HealthResponse get_health()

Service health check

Returns {"status":"ok"} when the service is running.

### Example


```python
import aimee_kb
from aimee_kb.models.health_response import HealthResponse
from aimee_kb.rest import ApiException
from pprint import pprint

# Defining the host is optional and defaults to http://127.0.0.1:8090/v1
# See configuration.py for a list of all supported configuration parameters.
configuration = aimee_kb.Configuration(
    host = "http://127.0.0.1:8090/v1"
)


# Enter a context with an instance of the API client
with aimee_kb.ApiClient(configuration) as api_client:
    # Create an instance of the API class
    api_instance = aimee_kb.DefaultApi(api_client)

    try:
        # Service health check
        api_response = api_instance.get_health()
        print("The response of DefaultApi->get_health:\n")
        pprint(api_response)
    except Exception as e:
        print("Exception when calling DefaultApi->get_health: %s\n" % e)
```



### Parameters

This endpoint does not need any parameter.

### Return type

[**HealthResponse**](HealthResponse.md)

### Authorization

No authorization required

### HTTP request headers

 - **Content-Type**: Not defined
 - **Accept**: application/json

### HTTP response details

| Status code | Description | Response headers |
|-------------|-------------|------------------|
**200** | Service is healthy |  -  |

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)

# **get_ingest_status**
> IngestStatusResponse get_ingest_status()

Report background project ingest status

### Example

* Bearer Authentication (bearerAuth):

```python
import aimee_kb
from aimee_kb.models.ingest_status_response import IngestStatusResponse
from aimee_kb.rest import ApiException
from pprint import pprint

# Defining the host is optional and defaults to http://127.0.0.1:8090/v1
# See configuration.py for a list of all supported configuration parameters.
configuration = aimee_kb.Configuration(
    host = "http://127.0.0.1:8090/v1"
)

# The client must configure the authentication and authorization parameters
# in accordance with the API server security policy.
# Examples for each auth method are provided below, use the example that
# satisfies your auth use case.

# Configure Bearer authorization: bearerAuth
configuration = aimee_kb.Configuration(
    access_token = os.environ["BEARER_TOKEN"]
)

# Enter a context with an instance of the API client
with aimee_kb.ApiClient(configuration) as api_client:
    # Create an instance of the API class
    api_instance = aimee_kb.DefaultApi(api_client)

    try:
        # Report background project ingest status
        api_response = api_instance.get_ingest_status()
        print("The response of DefaultApi->get_ingest_status:\n")
        pprint(api_response)
    except Exception as e:
        print("Exception when calling DefaultApi->get_ingest_status: %s\n" % e)
```



### Parameters

This endpoint does not need any parameter.

### Return type

[**IngestStatusResponse**](IngestStatusResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

 - **Content-Type**: Not defined
 - **Accept**: application/json

### HTTP response details

| Status code | Description | Response headers |
|-------------|-------------|------------------|
**200** | Background ingest status |  -  |
**401** | Unauthorized |  -  |
**405** | Method not allowed |  -  |
**503** | Ingest status unavailable |  -  |

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)

# **get_intelligence_bandit_export**
> object get_intelligence_bandit_export()

Export fusion bandit decision data

### Example

* Bearer Authentication (bearerAuth):

```python
import aimee_kb
from aimee_kb.rest import ApiException
from pprint import pprint

# Defining the host is optional and defaults to http://127.0.0.1:8090/v1
# See configuration.py for a list of all supported configuration parameters.
configuration = aimee_kb.Configuration(
    host = "http://127.0.0.1:8090/v1"
)

# The client must configure the authentication and authorization parameters
# in accordance with the API server security policy.
# Examples for each auth method are provided below, use the example that
# satisfies your auth use case.

# Configure Bearer authorization: bearerAuth
configuration = aimee_kb.Configuration(
    access_token = os.environ["BEARER_TOKEN"]
)

# Enter a context with an instance of the API client
with aimee_kb.ApiClient(configuration) as api_client:
    # Create an instance of the API class
    api_instance = aimee_kb.DefaultApi(api_client)

    try:
        # Export fusion bandit decision data
        api_response = api_instance.get_intelligence_bandit_export()
        print("The response of DefaultApi->get_intelligence_bandit_export:\n")
        pprint(api_response)
    except Exception as e:
        print("Exception when calling DefaultApi->get_intelligence_bandit_export: %s\n" % e)
```



### Parameters

This endpoint does not need any parameter.

### Return type

**object**

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

 - **Content-Type**: Not defined
 - **Accept**: application/json

### HTTP response details

| Status code | Description | Response headers |
|-------------|-------------|------------------|
**200** | Bandit decisions and arm stats |  -  |
**401** | Unauthorized |  -  |

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)

# **get_intelligence_calibration_readiness**
> object get_intelligence_calibration_readiness()

Calibration readiness

### Example

* Bearer Authentication (bearerAuth):

```python
import aimee_kb
from aimee_kb.rest import ApiException
from pprint import pprint

# Defining the host is optional and defaults to http://127.0.0.1:8090/v1
# See configuration.py for a list of all supported configuration parameters.
configuration = aimee_kb.Configuration(
    host = "http://127.0.0.1:8090/v1"
)

# The client must configure the authentication and authorization parameters
# in accordance with the API server security policy.
# Examples for each auth method are provided below, use the example that
# satisfies your auth use case.

# Configure Bearer authorization: bearerAuth
configuration = aimee_kb.Configuration(
    access_token = os.environ["BEARER_TOKEN"]
)

# Enter a context with an instance of the API client
with aimee_kb.ApiClient(configuration) as api_client:
    # Create an instance of the API class
    api_instance = aimee_kb.DefaultApi(api_client)

    try:
        # Calibration readiness
        api_response = api_instance.get_intelligence_calibration_readiness()
        print("The response of DefaultApi->get_intelligence_calibration_readiness:\n")
        pprint(api_response)
    except Exception as e:
        print("Exception when calling DefaultApi->get_intelligence_calibration_readiness: %s\n" % e)
```



### Parameters

This endpoint does not need any parameter.

### Return type

**object**

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

 - **Content-Type**: Not defined
 - **Accept**: application/json

### HTTP response details

| Status code | Description | Response headers |
|-------------|-------------|------------------|
**200** | Calibration readiness summary |  -  |
**401** | Unauthorized |  -  |

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)

# **get_intelligence_demotion_check**
> object get_intelligence_demotion_check()

Dry-run demotion readiness check

### Example

* Bearer Authentication (bearerAuth):

```python
import aimee_kb
from aimee_kb.rest import ApiException
from pprint import pprint

# Defining the host is optional and defaults to http://127.0.0.1:8090/v1
# See configuration.py for a list of all supported configuration parameters.
configuration = aimee_kb.Configuration(
    host = "http://127.0.0.1:8090/v1"
)

# The client must configure the authentication and authorization parameters
# in accordance with the API server security policy.
# Examples for each auth method are provided below, use the example that
# satisfies your auth use case.

# Configure Bearer authorization: bearerAuth
configuration = aimee_kb.Configuration(
    access_token = os.environ["BEARER_TOKEN"]
)

# Enter a context with an instance of the API client
with aimee_kb.ApiClient(configuration) as api_client:
    # Create an instance of the API class
    api_instance = aimee_kb.DefaultApi(api_client)

    try:
        # Dry-run demotion readiness check
        api_response = api_instance.get_intelligence_demotion_check()
        print("The response of DefaultApi->get_intelligence_demotion_check:\n")
        pprint(api_response)
    except Exception as e:
        print("Exception when calling DefaultApi->get_intelligence_demotion_check: %s\n" % e)
```



### Parameters

This endpoint does not need any parameter.

### Return type

**object**

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

 - **Content-Type**: Not defined
 - **Accept**: application/json

### HTTP response details

| Status code | Description | Response headers |
|-------------|-------------|------------------|
**200** | Demotion readiness summary |  -  |
**401** | Unauthorized |  -  |

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)

# **get_job_status**
> JobStatusResponse get_job_status(job_id)

Report asynchronous knowledge ingest job status

### Example

* Bearer Authentication (bearerAuth):

```python
import aimee_kb
from aimee_kb.models.job_status_response import JobStatusResponse
from aimee_kb.rest import ApiException
from pprint import pprint

# Defining the host is optional and defaults to http://127.0.0.1:8090/v1
# See configuration.py for a list of all supported configuration parameters.
configuration = aimee_kb.Configuration(
    host = "http://127.0.0.1:8090/v1"
)

# The client must configure the authentication and authorization parameters
# in accordance with the API server security policy.
# Examples for each auth method are provided below, use the example that
# satisfies your auth use case.

# Configure Bearer authorization: bearerAuth
configuration = aimee_kb.Configuration(
    access_token = os.environ["BEARER_TOKEN"]
)

# Enter a context with an instance of the API client
with aimee_kb.ApiClient(configuration) as api_client:
    # Create an instance of the API class
    api_instance = aimee_kb.DefaultApi(api_client)
    job_id = 56 # int | 

    try:
        # Report asynchronous knowledge ingest job status
        api_response = api_instance.get_job_status(job_id)
        print("The response of DefaultApi->get_job_status:\n")
        pprint(api_response)
    except Exception as e:
        print("Exception when calling DefaultApi->get_job_status: %s\n" % e)
```



### Parameters


Name | Type | Description  | Notes
------------- | ------------- | ------------- | -------------
 **job_id** | **int**|  | 

### Return type

[**JobStatusResponse**](JobStatusResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

 - **Content-Type**: Not defined
 - **Accept**: application/json

### HTTP response details

| Status code | Description | Response headers |
|-------------|-------------|------------------|
**200** | Job status |  -  |
**401** | Unauthorized |  -  |
**404** | Job not found |  -  |
**405** | Method not allowed |  -  |
**503** | Job status unavailable |  -  |

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)

# **get_pipeline_status**
> PipelineStatusResponse get_pipeline_status()

Report asynchronous knowledge ingest queue status

### Example

* Bearer Authentication (bearerAuth):

```python
import aimee_kb
from aimee_kb.models.pipeline_status_response import PipelineStatusResponse
from aimee_kb.rest import ApiException
from pprint import pprint

# Defining the host is optional and defaults to http://127.0.0.1:8090/v1
# See configuration.py for a list of all supported configuration parameters.
configuration = aimee_kb.Configuration(
    host = "http://127.0.0.1:8090/v1"
)

# The client must configure the authentication and authorization parameters
# in accordance with the API server security policy.
# Examples for each auth method are provided below, use the example that
# satisfies your auth use case.

# Configure Bearer authorization: bearerAuth
configuration = aimee_kb.Configuration(
    access_token = os.environ["BEARER_TOKEN"]
)

# Enter a context with an instance of the API client
with aimee_kb.ApiClient(configuration) as api_client:
    # Create an instance of the API class
    api_instance = aimee_kb.DefaultApi(api_client)

    try:
        # Report asynchronous knowledge ingest queue status
        api_response = api_instance.get_pipeline_status()
        print("The response of DefaultApi->get_pipeline_status:\n")
        pprint(api_response)
    except Exception as e:
        print("Exception when calling DefaultApi->get_pipeline_status: %s\n" % e)
```



### Parameters

This endpoint does not need any parameter.

### Return type

[**PipelineStatusResponse**](PipelineStatusResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

 - **Content-Type**: Not defined
 - **Accept**: application/json

### HTTP response details

| Status code | Description | Response headers |
|-------------|-------------|------------------|
**200** | Pipeline status |  -  |
**401** | Unauthorized |  -  |
**405** | Method not allowed |  -  |
**503** | Queue unavailable |  -  |

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)

# **get_review**
> ReviewQueueResponse get_review(cursor=cursor, limit=limit)

List staged documents pending review

### Example

* Bearer Authentication (bearerAuth):

```python
import aimee_kb
from aimee_kb.models.review_queue_response import ReviewQueueResponse
from aimee_kb.rest import ApiException
from pprint import pprint

# Defining the host is optional and defaults to http://127.0.0.1:8090/v1
# See configuration.py for a list of all supported configuration parameters.
configuration = aimee_kb.Configuration(
    host = "http://127.0.0.1:8090/v1"
)

# The client must configure the authentication and authorization parameters
# in accordance with the API server security policy.
# Examples for each auth method are provided below, use the example that
# satisfies your auth use case.

# Configure Bearer authorization: bearerAuth
configuration = aimee_kb.Configuration(
    access_token = os.environ["BEARER_TOKEN"]
)

# Enter a context with an instance of the API client
with aimee_kb.ApiClient(configuration) as api_client:
    # Create an instance of the API class
    api_instance = aimee_kb.DefaultApi(api_client)
    cursor = 56 # int |  (optional)
    limit = 10 # int |  (optional) (default to 10)

    try:
        # List staged documents pending review
        api_response = api_instance.get_review(cursor=cursor, limit=limit)
        print("The response of DefaultApi->get_review:\n")
        pprint(api_response)
    except Exception as e:
        print("Exception when calling DefaultApi->get_review: %s\n" % e)
```



### Parameters


Name | Type | Description  | Notes
------------- | ------------- | ------------- | -------------
 **cursor** | **int**|  | [optional] 
 **limit** | **int**|  | [optional] [default to 10]

### Return type

[**ReviewQueueResponse**](ReviewQueueResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

 - **Content-Type**: Not defined
 - **Accept**: application/json

### HTTP response details

| Status code | Description | Response headers |
|-------------|-------------|------------------|
**200** | Review queue |  -  |
**401** | Unauthorized |  -  |

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)

# **get_version**
> VersionResponse get_version()

Service version

### Example

* Bearer Authentication (bearerAuth):

```python
import aimee_kb
from aimee_kb.models.version_response import VersionResponse
from aimee_kb.rest import ApiException
from pprint import pprint

# Defining the host is optional and defaults to http://127.0.0.1:8090/v1
# See configuration.py for a list of all supported configuration parameters.
configuration = aimee_kb.Configuration(
    host = "http://127.0.0.1:8090/v1"
)

# The client must configure the authentication and authorization parameters
# in accordance with the API server security policy.
# Examples for each auth method are provided below, use the example that
# satisfies your auth use case.

# Configure Bearer authorization: bearerAuth
configuration = aimee_kb.Configuration(
    access_token = os.environ["BEARER_TOKEN"]
)

# Enter a context with an instance of the API client
with aimee_kb.ApiClient(configuration) as api_client:
    # Create an instance of the API class
    api_instance = aimee_kb.DefaultApi(api_client)

    try:
        # Service version
        api_response = api_instance.get_version()
        print("The response of DefaultApi->get_version:\n")
        pprint(api_response)
    except Exception as e:
        print("Exception when calling DefaultApi->get_version: %s\n" % e)
```



### Parameters

This endpoint does not need any parameter.

### Return type

[**VersionResponse**](VersionResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

 - **Content-Type**: Not defined
 - **Accept**: application/json

### HTTP response details

| Status code | Description | Response headers |
|-------------|-------------|------------------|
**200** | Version information |  -  |
**401** | Unauthorized |  -  |

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)

# **get_workers**
> WorkersResponse get_workers()

Report aimee-kb worker and background task status

### Example

* Bearer Authentication (bearerAuth):

```python
import aimee_kb
from aimee_kb.models.workers_response import WorkersResponse
from aimee_kb.rest import ApiException
from pprint import pprint

# Defining the host is optional and defaults to http://127.0.0.1:8090/v1
# See configuration.py for a list of all supported configuration parameters.
configuration = aimee_kb.Configuration(
    host = "http://127.0.0.1:8090/v1"
)

# The client must configure the authentication and authorization parameters
# in accordance with the API server security policy.
# Examples for each auth method are provided below, use the example that
# satisfies your auth use case.

# Configure Bearer authorization: bearerAuth
configuration = aimee_kb.Configuration(
    access_token = os.environ["BEARER_TOKEN"]
)

# Enter a context with an instance of the API client
with aimee_kb.ApiClient(configuration) as api_client:
    # Create an instance of the API class
    api_instance = aimee_kb.DefaultApi(api_client)

    try:
        # Report aimee-kb worker and background task status
        api_response = api_instance.get_workers()
        print("The response of DefaultApi->get_workers:\n")
        pprint(api_response)
    except Exception as e:
        print("Exception when calling DefaultApi->get_workers: %s\n" % e)
```



### Parameters

This endpoint does not need any parameter.

### Return type

[**WorkersResponse**](WorkersResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

 - **Content-Type**: Not defined
 - **Accept**: application/json

### HTTP response details

| Status code | Description | Response headers |
|-------------|-------------|------------------|
**200** | Worker status |  -  |
**401** | Unauthorized |  -  |
**405** | Method not allowed |  -  |
**503** | Workers unavailable |  -  |

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)

# **head_health**
> head_health()

Service health check (HEAD)

### Example


```python
import aimee_kb
from aimee_kb.rest import ApiException
from pprint import pprint

# Defining the host is optional and defaults to http://127.0.0.1:8090/v1
# See configuration.py for a list of all supported configuration parameters.
configuration = aimee_kb.Configuration(
    host = "http://127.0.0.1:8090/v1"
)


# Enter a context with an instance of the API client
with aimee_kb.ApiClient(configuration) as api_client:
    # Create an instance of the API class
    api_instance = aimee_kb.DefaultApi(api_client)

    try:
        # Service health check (HEAD)
        api_instance.head_health()
    except Exception as e:
        print("Exception when calling DefaultApi->head_health: %s\n" % e)
```



### Parameters

This endpoint does not need any parameter.

### Return type

void (empty response body)

### Authorization

No authorization required

### HTTP request headers

 - **Content-Type**: Not defined
 - **Accept**: Not defined

### HTTP response details

| Status code | Description | Response headers |
|-------------|-------------|------------------|
**200** | Service is healthy |  -  |

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)

# **post_action**
> object post_action(action, body=body)

Execute a versioned knowledge-service action

### Example

* Bearer Authentication (bearerAuth):

```python
import aimee_kb
from aimee_kb.rest import ApiException
from pprint import pprint

# Defining the host is optional and defaults to http://127.0.0.1:8090/v1
# See configuration.py for a list of all supported configuration parameters.
configuration = aimee_kb.Configuration(
    host = "http://127.0.0.1:8090/v1"
)

# The client must configure the authentication and authorization parameters
# in accordance with the API server security policy.
# Examples for each auth method are provided below, use the example that
# satisfies your auth use case.

# Configure Bearer authorization: bearerAuth
configuration = aimee_kb.Configuration(
    access_token = os.environ["BEARER_TOKEN"]
)

# Enter a context with an instance of the API client
with aimee_kb.ApiClient(configuration) as api_client:
    # Create an instance of the API class
    api_instance = aimee_kb.DefaultApi(api_client)
    action = 'action_example' # str | 
    body = None # object |  (optional)

    try:
        # Execute a versioned knowledge-service action
        api_response = api_instance.post_action(action, body=body)
        print("The response of DefaultApi->post_action:\n")
        pprint(api_response)
    except Exception as e:
        print("Exception when calling DefaultApi->post_action: %s\n" % e)
```



### Parameters


Name | Type | Description  | Notes
------------- | ------------- | ------------- | -------------
 **action** | **str**|  | 
 **body** | **object**|  | [optional] 

### Return type

**object**

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

 - **Content-Type**: application/json
 - **Accept**: application/json

### HTTP response details

| Status code | Description | Response headers |
|-------------|-------------|------------------|
**200** | Action response |  -  |
**401** | Unauthorized |  -  |
**404** | Unknown action |  -  |

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)

# **post_code_build**
> CodeBuildResponse post_code_build(code_build_request)

Build a project knowledge index

### Example

* Bearer Authentication (bearerAuth):

```python
import aimee_kb
from aimee_kb.models.code_build_request import CodeBuildRequest
from aimee_kb.models.code_build_response import CodeBuildResponse
from aimee_kb.rest import ApiException
from pprint import pprint

# Defining the host is optional and defaults to http://127.0.0.1:8090/v1
# See configuration.py for a list of all supported configuration parameters.
configuration = aimee_kb.Configuration(
    host = "http://127.0.0.1:8090/v1"
)

# The client must configure the authentication and authorization parameters
# in accordance with the API server security policy.
# Examples for each auth method are provided below, use the example that
# satisfies your auth use case.

# Configure Bearer authorization: bearerAuth
configuration = aimee_kb.Configuration(
    access_token = os.environ["BEARER_TOKEN"]
)

# Enter a context with an instance of the API client
with aimee_kb.ApiClient(configuration) as api_client:
    # Create an instance of the API class
    api_instance = aimee_kb.DefaultApi(api_client)
    code_build_request = aimee_kb.CodeBuildRequest() # CodeBuildRequest | 

    try:
        # Build a project knowledge index
        api_response = api_instance.post_code_build(code_build_request)
        print("The response of DefaultApi->post_code_build:\n")
        pprint(api_response)
    except Exception as e:
        print("Exception when calling DefaultApi->post_code_build: %s\n" % e)
```



### Parameters


Name | Type | Description  | Notes
------------- | ------------- | ------------- | -------------
 **code_build_request** | [**CodeBuildRequest**](CodeBuildRequest.md)|  | 

### Return type

[**CodeBuildResponse**](CodeBuildResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

 - **Content-Type**: application/json
 - **Accept**: application/json

### HTTP response details

| Status code | Description | Response headers |
|-------------|-------------|------------------|
**200** | Build complete |  -  |
**400** | Missing required parameters |  -  |
**401** | Unauthorized |  -  |
**503** | Knowledge or vector store unavailable |  -  |

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)

# **post_code_scan**
> CodeScanResponse post_code_scan(code_scan_request)

Request a canonical code index scan

### Example

* Bearer Authentication (bearerAuth):

```python
import aimee_kb
from aimee_kb.models.code_scan_request import CodeScanRequest
from aimee_kb.models.code_scan_response import CodeScanResponse
from aimee_kb.rest import ApiException
from pprint import pprint

# Defining the host is optional and defaults to http://127.0.0.1:8090/v1
# See configuration.py for a list of all supported configuration parameters.
configuration = aimee_kb.Configuration(
    host = "http://127.0.0.1:8090/v1"
)

# The client must configure the authentication and authorization parameters
# in accordance with the API server security policy.
# Examples for each auth method are provided below, use the example that
# satisfies your auth use case.

# Configure Bearer authorization: bearerAuth
configuration = aimee_kb.Configuration(
    access_token = os.environ["BEARER_TOKEN"]
)

# Enter a context with an instance of the API client
with aimee_kb.ApiClient(configuration) as api_client:
    # Create an instance of the API class
    api_instance = aimee_kb.DefaultApi(api_client)
    code_scan_request = aimee_kb.CodeScanRequest() # CodeScanRequest | 

    try:
        # Request a canonical code index scan
        api_response = api_instance.post_code_scan(code_scan_request)
        print("The response of DefaultApi->post_code_scan:\n")
        pprint(api_response)
    except Exception as e:
        print("Exception when calling DefaultApi->post_code_scan: %s\n" % e)
```



### Parameters


Name | Type | Description  | Notes
------------- | ------------- | ------------- | -------------
 **code_scan_request** | [**CodeScanRequest**](CodeScanRequest.md)|  | 

### Return type

[**CodeScanResponse**](CodeScanResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

 - **Content-Type**: application/json
 - **Accept**: application/json

### HTTP response details

| Status code | Description | Response headers |
|-------------|-------------|------------------|
**202** | Scan accepted |  -  |
**401** | Unauthorized |  -  |
**405** | Method not allowed |  -  |

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)

# **post_code_update**
> CodeUpdateResponse post_code_update(code_update_request)

Incrementally update a project knowledge index

### Example

* Bearer Authentication (bearerAuth):

```python
import aimee_kb
from aimee_kb.models.code_update_request import CodeUpdateRequest
from aimee_kb.models.code_update_response import CodeUpdateResponse
from aimee_kb.rest import ApiException
from pprint import pprint

# Defining the host is optional and defaults to http://127.0.0.1:8090/v1
# See configuration.py for a list of all supported configuration parameters.
configuration = aimee_kb.Configuration(
    host = "http://127.0.0.1:8090/v1"
)

# The client must configure the authentication and authorization parameters
# in accordance with the API server security policy.
# Examples for each auth method are provided below, use the example that
# satisfies your auth use case.

# Configure Bearer authorization: bearerAuth
configuration = aimee_kb.Configuration(
    access_token = os.environ["BEARER_TOKEN"]
)

# Enter a context with an instance of the API client
with aimee_kb.ApiClient(configuration) as api_client:
    # Create an instance of the API class
    api_instance = aimee_kb.DefaultApi(api_client)
    code_update_request = aimee_kb.CodeUpdateRequest() # CodeUpdateRequest | 

    try:
        # Incrementally update a project knowledge index
        api_response = api_instance.post_code_update(code_update_request)
        print("The response of DefaultApi->post_code_update:\n")
        pprint(api_response)
    except Exception as e:
        print("Exception when calling DefaultApi->post_code_update: %s\n" % e)
```



### Parameters


Name | Type | Description  | Notes
------------- | ------------- | ------------- | -------------
 **code_update_request** | [**CodeUpdateRequest**](CodeUpdateRequest.md)|  | 

### Return type

[**CodeUpdateResponse**](CodeUpdateResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

 - **Content-Type**: application/json
 - **Accept**: application/json

### HTTP response details

| Status code | Description | Response headers |
|-------------|-------------|------------------|
**200** | Update complete |  -  |
**400** | Missing required parameters |  -  |
**401** | Unauthorized |  -  |
**503** | Knowledge store unavailable |  -  |

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)

# **post_docs**
> DocIngestResponse post_docs(file, scope=scope)

Upload a document for ingest

Accepts multipart/form-data with a required `file` part and an optional
`scope` field (default: global). Normalizes to markdown, stores in DB2,
and queues an extract_doc job. Returns existing doc_id on idempotent re-upload.


### Example

* Bearer Authentication (bearerAuth):

```python
import aimee_kb
from aimee_kb.models.doc_ingest_response import DocIngestResponse
from aimee_kb.rest import ApiException
from pprint import pprint

# Defining the host is optional and defaults to http://127.0.0.1:8090/v1
# See configuration.py for a list of all supported configuration parameters.
configuration = aimee_kb.Configuration(
    host = "http://127.0.0.1:8090/v1"
)

# The client must configure the authentication and authorization parameters
# in accordance with the API server security policy.
# Examples for each auth method are provided below, use the example that
# satisfies your auth use case.

# Configure Bearer authorization: bearerAuth
configuration = aimee_kb.Configuration(
    access_token = os.environ["BEARER_TOKEN"]
)

# Enter a context with an instance of the API client
with aimee_kb.ApiClient(configuration) as api_client:
    # Create an instance of the API class
    api_instance = aimee_kb.DefaultApi(api_client)
    file = None # bytes | 
    scope = 'global' # str |  (optional) (default to 'global')

    try:
        # Upload a document for ingest
        api_response = api_instance.post_docs(file, scope=scope)
        print("The response of DefaultApi->post_docs:\n")
        pprint(api_response)
    except Exception as e:
        print("Exception when calling DefaultApi->post_docs: %s\n" % e)
```



### Parameters


Name | Type | Description  | Notes
------------- | ------------- | ------------- | -------------
 **file** | **bytes**|  | 
 **scope** | **str**|  | [optional] [default to &#39;global&#39;]

### Return type

[**DocIngestResponse**](DocIngestResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

 - **Content-Type**: multipart/form-data
 - **Accept**: application/json

### HTTP response details

| Status code | Description | Response headers |
|-------------|-------------|------------------|
**201** | Document ingested |  -  |
**200** | Idempotent re-upload (existing doc_id returned) |  -  |
**400** | Missing file part |  -  |
**401** | Unauthorized |  -  |
**422** | Normalization failed (converter error) |  -  |
**503** | DB unavailable |  -  |

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)

# **post_docs_manifest**
> DocsManifestResponse post_docs_manifest(docs_manifest_request)

Check which uploaded document hashes are absent

### Example

* Bearer Authentication (bearerAuth):

```python
import aimee_kb
from aimee_kb.models.docs_manifest_request import DocsManifestRequest
from aimee_kb.models.docs_manifest_response import DocsManifestResponse
from aimee_kb.rest import ApiException
from pprint import pprint

# Defining the host is optional and defaults to http://127.0.0.1:8090/v1
# See configuration.py for a list of all supported configuration parameters.
configuration = aimee_kb.Configuration(
    host = "http://127.0.0.1:8090/v1"
)

# The client must configure the authentication and authorization parameters
# in accordance with the API server security policy.
# Examples for each auth method are provided below, use the example that
# satisfies your auth use case.

# Configure Bearer authorization: bearerAuth
configuration = aimee_kb.Configuration(
    access_token = os.environ["BEARER_TOKEN"]
)

# Enter a context with an instance of the API client
with aimee_kb.ApiClient(configuration) as api_client:
    # Create an instance of the API class
    api_instance = aimee_kb.DefaultApi(api_client)
    docs_manifest_request = aimee_kb.DocsManifestRequest() # DocsManifestRequest | 

    try:
        # Check which uploaded document hashes are absent
        api_response = api_instance.post_docs_manifest(docs_manifest_request)
        print("The response of DefaultApi->post_docs_manifest:\n")
        pprint(api_response)
    except Exception as e:
        print("Exception when calling DefaultApi->post_docs_manifest: %s\n" % e)
```



### Parameters


Name | Type | Description  | Notes
------------- | ------------- | ------------- | -------------
 **docs_manifest_request** | [**DocsManifestRequest**](DocsManifestRequest.md)|  | 

### Return type

[**DocsManifestResponse**](DocsManifestResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

 - **Content-Type**: application/json
 - **Accept**: application/json

### HTTP response details

| Status code | Description | Response headers |
|-------------|-------------|------------------|
**200** | Manifest diff |  -  |
**400** | Invalid manifest request |  -  |
**401** | Unauthorized |  -  |
**405** | Method not allowed |  -  |
**500** | Serialization failed |  -  |
**503** | DB unavailable |  -  |

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)

# **post_drain**
> DrainResponse post_drain(drain_request=drain_request)

Drain the asynchronous knowledge ingest queue

### Example

* Bearer Authentication (bearerAuth):

```python
import aimee_kb
from aimee_kb.models.drain_request import DrainRequest
from aimee_kb.models.drain_response import DrainResponse
from aimee_kb.rest import ApiException
from pprint import pprint

# Defining the host is optional and defaults to http://127.0.0.1:8090/v1
# See configuration.py for a list of all supported configuration parameters.
configuration = aimee_kb.Configuration(
    host = "http://127.0.0.1:8090/v1"
)

# The client must configure the authentication and authorization parameters
# in accordance with the API server security policy.
# Examples for each auth method are provided below, use the example that
# satisfies your auth use case.

# Configure Bearer authorization: bearerAuth
configuration = aimee_kb.Configuration(
    access_token = os.environ["BEARER_TOKEN"]
)

# Enter a context with an instance of the API client
with aimee_kb.ApiClient(configuration) as api_client:
    # Create an instance of the API class
    api_instance = aimee_kb.DefaultApi(api_client)
    drain_request = aimee_kb.DrainRequest() # DrainRequest |  (optional)

    try:
        # Drain the asynchronous knowledge ingest queue
        api_response = api_instance.post_drain(drain_request=drain_request)
        print("The response of DefaultApi->post_drain:\n")
        pprint(api_response)
    except Exception as e:
        print("Exception when calling DefaultApi->post_drain: %s\n" % e)
```



### Parameters


Name | Type | Description  | Notes
------------- | ------------- | ------------- | -------------
 **drain_request** | [**DrainRequest**](DrainRequest.md)|  | [optional] 

### Return type

[**DrainResponse**](DrainResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

 - **Content-Type**: application/json
 - **Accept**: application/json

### HTTP response details

| Status code | Description | Response headers |
|-------------|-------------|------------------|
**200** | Queue drained |  -  |
**401** | Unauthorized |  -  |
**405** | Method not allowed |  -  |
**500** | Queue drain failed |  -  |

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)

# **post_entity_search**
> EntitySearchResponse post_entity_search(entity_search_request)

Find entities by name or context

### Example

* Bearer Authentication (bearerAuth):

```python
import aimee_kb
from aimee_kb.models.entity_search_request import EntitySearchRequest
from aimee_kb.models.entity_search_response import EntitySearchResponse
from aimee_kb.rest import ApiException
from pprint import pprint

# Defining the host is optional and defaults to http://127.0.0.1:8090/v1
# See configuration.py for a list of all supported configuration parameters.
configuration = aimee_kb.Configuration(
    host = "http://127.0.0.1:8090/v1"
)

# The client must configure the authentication and authorization parameters
# in accordance with the API server security policy.
# Examples for each auth method are provided below, use the example that
# satisfies your auth use case.

# Configure Bearer authorization: bearerAuth
configuration = aimee_kb.Configuration(
    access_token = os.environ["BEARER_TOKEN"]
)

# Enter a context with an instance of the API client
with aimee_kb.ApiClient(configuration) as api_client:
    # Create an instance of the API class
    api_instance = aimee_kb.DefaultApi(api_client)
    entity_search_request = aimee_kb.EntitySearchRequest() # EntitySearchRequest | 

    try:
        # Find entities by name or context
        api_response = api_instance.post_entity_search(entity_search_request)
        print("The response of DefaultApi->post_entity_search:\n")
        pprint(api_response)
    except Exception as e:
        print("Exception when calling DefaultApi->post_entity_search: %s\n" % e)
```



### Parameters


Name | Type | Description  | Notes
------------- | ------------- | ------------- | -------------
 **entity_search_request** | [**EntitySearchRequest**](EntitySearchRequest.md)|  | 

### Return type

[**EntitySearchResponse**](EntitySearchResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

 - **Content-Type**: application/json
 - **Accept**: application/json

### HTTP response details

| Status code | Description | Response headers |
|-------------|-------------|------------------|
**200** | Entity search results |  -  |
**400** | Missing query |  -  |
**401** | Unauthorized |  -  |

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)

# **post_ingest**
> IngestResponse post_ingest(ingest_request=ingest_request)

Enqueue background project ingest

### Example

* Bearer Authentication (bearerAuth):

```python
import aimee_kb
from aimee_kb.models.ingest_request import IngestRequest
from aimee_kb.models.ingest_response import IngestResponse
from aimee_kb.rest import ApiException
from pprint import pprint

# Defining the host is optional and defaults to http://127.0.0.1:8090/v1
# See configuration.py for a list of all supported configuration parameters.
configuration = aimee_kb.Configuration(
    host = "http://127.0.0.1:8090/v1"
)

# The client must configure the authentication and authorization parameters
# in accordance with the API server security policy.
# Examples for each auth method are provided below, use the example that
# satisfies your auth use case.

# Configure Bearer authorization: bearerAuth
configuration = aimee_kb.Configuration(
    access_token = os.environ["BEARER_TOKEN"]
)

# Enter a context with an instance of the API client
with aimee_kb.ApiClient(configuration) as api_client:
    # Create an instance of the API class
    api_instance = aimee_kb.DefaultApi(api_client)
    ingest_request = aimee_kb.IngestRequest() # IngestRequest |  (optional)

    try:
        # Enqueue background project ingest
        api_response = api_instance.post_ingest(ingest_request=ingest_request)
        print("The response of DefaultApi->post_ingest:\n")
        pprint(api_response)
    except Exception as e:
        print("Exception when calling DefaultApi->post_ingest: %s\n" % e)
```



### Parameters


Name | Type | Description  | Notes
------------- | ------------- | ------------- | -------------
 **ingest_request** | [**IngestRequest**](IngestRequest.md)|  | [optional] 

### Return type

[**IngestResponse**](IngestResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

 - **Content-Type**: application/json
 - **Accept**: application/json

### HTTP response details

| Status code | Description | Response headers |
|-------------|-------------|------------------|
**202** | Ingest queued |  -  |
**401** | Unauthorized |  -  |
**405** | Method not allowed |  -  |
**503** | Knowledge store unavailable |  -  |

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)

# **post_intelligence_bandit_close**
> object post_intelligence_bandit_close()

Close a sampled decision with its observed reward

### Example

* Bearer Authentication (bearerAuth):

```python
import aimee_kb
from aimee_kb.rest import ApiException
from pprint import pprint

# Defining the host is optional and defaults to http://127.0.0.1:8090/v1
# See configuration.py for a list of all supported configuration parameters.
configuration = aimee_kb.Configuration(
    host = "http://127.0.0.1:8090/v1"
)

# The client must configure the authentication and authorization parameters
# in accordance with the API server security policy.
# Examples for each auth method are provided below, use the example that
# satisfies your auth use case.

# Configure Bearer authorization: bearerAuth
configuration = aimee_kb.Configuration(
    access_token = os.environ["BEARER_TOKEN"]
)

# Enter a context with an instance of the API client
with aimee_kb.ApiClient(configuration) as api_client:
    # Create an instance of the API class
    api_instance = aimee_kb.DefaultApi(api_client)

    try:
        # Close a sampled decision with its observed reward
        api_response = api_instance.post_intelligence_bandit_close()
        print("The response of DefaultApi->post_intelligence_bandit_close:\n")
        pprint(api_response)
    except Exception as e:
        print("Exception when calling DefaultApi->post_intelligence_bandit_close: %s\n" % e)
```



### Parameters

This endpoint does not need any parameter.

### Return type

**object**

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

 - **Content-Type**: Not defined
 - **Accept**: application/json

### HTTP response details

| Status code | Description | Response headers |
|-------------|-------------|------------------|
**200** | Close result |  -  |
**401** | Unauthorized |  -  |

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)

# **post_intelligence_bandit_promote**
> object post_intelligence_bandit_promote()

Persist the production-default arm for a decision point

### Example

* Bearer Authentication (bearerAuth):

```python
import aimee_kb
from aimee_kb.rest import ApiException
from pprint import pprint

# Defining the host is optional and defaults to http://127.0.0.1:8090/v1
# See configuration.py for a list of all supported configuration parameters.
configuration = aimee_kb.Configuration(
    host = "http://127.0.0.1:8090/v1"
)

# The client must configure the authentication and authorization parameters
# in accordance with the API server security policy.
# Examples for each auth method are provided below, use the example that
# satisfies your auth use case.

# Configure Bearer authorization: bearerAuth
configuration = aimee_kb.Configuration(
    access_token = os.environ["BEARER_TOKEN"]
)

# Enter a context with an instance of the API client
with aimee_kb.ApiClient(configuration) as api_client:
    # Create an instance of the API class
    api_instance = aimee_kb.DefaultApi(api_client)

    try:
        # Persist the production-default arm for a decision point
        api_response = api_instance.post_intelligence_bandit_promote()
        print("The response of DefaultApi->post_intelligence_bandit_promote:\n")
        pprint(api_response)
    except Exception as e:
        print("Exception when calling DefaultApi->post_intelligence_bandit_promote: %s\n" % e)
```



### Parameters

This endpoint does not need any parameter.

### Return type

**object**

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

 - **Content-Type**: Not defined
 - **Accept**: application/json

### HTTP response details

| Status code | Description | Response headers |
|-------------|-------------|------------------|
**200** | Promotion result (rollback_arm) |  -  |
**401** | Unauthorized |  -  |

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)

# **post_intelligence_bandit_sample**
> object post_intelligence_bandit_sample()

Sample an arm for a decision point (server-side decision points)

### Example

* Bearer Authentication (bearerAuth):

```python
import aimee_kb
from aimee_kb.rest import ApiException
from pprint import pprint

# Defining the host is optional and defaults to http://127.0.0.1:8090/v1
# See configuration.py for a list of all supported configuration parameters.
configuration = aimee_kb.Configuration(
    host = "http://127.0.0.1:8090/v1"
)

# The client must configure the authentication and authorization parameters
# in accordance with the API server security policy.
# Examples for each auth method are provided below, use the example that
# satisfies your auth use case.

# Configure Bearer authorization: bearerAuth
configuration = aimee_kb.Configuration(
    access_token = os.environ["BEARER_TOKEN"]
)

# Enter a context with an instance of the API client
with aimee_kb.ApiClient(configuration) as api_client:
    # Create an instance of the API class
    api_instance = aimee_kb.DefaultApi(api_client)

    try:
        # Sample an arm for a decision point (server-side decision points)
        api_response = api_instance.post_intelligence_bandit_sample()
        print("The response of DefaultApi->post_intelligence_bandit_sample:\n")
        pprint(api_response)
    except Exception as e:
        print("Exception when calling DefaultApi->post_intelligence_bandit_sample: %s\n" % e)
```



### Parameters

This endpoint does not need any parameter.

### Return type

**object**

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

 - **Content-Type**: Not defined
 - **Accept**: application/json

### HTTP response details

| Status code | Description | Response headers |
|-------------|-------------|------------------|
**200** | Selected arm + decision id (or status disabled) |  -  |
**401** | Unauthorized |  -  |

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)

# **post_maintenance_clear**
> MaintenanceClearResponse post_maintenance_clear(maintenance_clear_request)

Clear indexed knowledge for a project

### Example

* Bearer Authentication (bearerAuth):

```python
import aimee_kb
from aimee_kb.models.maintenance_clear_request import MaintenanceClearRequest
from aimee_kb.models.maintenance_clear_response import MaintenanceClearResponse
from aimee_kb.rest import ApiException
from pprint import pprint

# Defining the host is optional and defaults to http://127.0.0.1:8090/v1
# See configuration.py for a list of all supported configuration parameters.
configuration = aimee_kb.Configuration(
    host = "http://127.0.0.1:8090/v1"
)

# The client must configure the authentication and authorization parameters
# in accordance with the API server security policy.
# Examples for each auth method are provided below, use the example that
# satisfies your auth use case.

# Configure Bearer authorization: bearerAuth
configuration = aimee_kb.Configuration(
    access_token = os.environ["BEARER_TOKEN"]
)

# Enter a context with an instance of the API client
with aimee_kb.ApiClient(configuration) as api_client:
    # Create an instance of the API class
    api_instance = aimee_kb.DefaultApi(api_client)
    maintenance_clear_request = aimee_kb.MaintenanceClearRequest() # MaintenanceClearRequest | 

    try:
        # Clear indexed knowledge for a project
        api_response = api_instance.post_maintenance_clear(maintenance_clear_request)
        print("The response of DefaultApi->post_maintenance_clear:\n")
        pprint(api_response)
    except Exception as e:
        print("Exception when calling DefaultApi->post_maintenance_clear: %s\n" % e)
```



### Parameters


Name | Type | Description  | Notes
------------- | ------------- | ------------- | -------------
 **maintenance_clear_request** | [**MaintenanceClearRequest**](MaintenanceClearRequest.md)|  | 

### Return type

[**MaintenanceClearResponse**](MaintenanceClearResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

 - **Content-Type**: application/json
 - **Accept**: application/json

### HTTP response details

| Status code | Description | Response headers |
|-------------|-------------|------------------|
**200** | Project cleared |  -  |
**400** | Missing project |  -  |
**401** | Unauthorized |  -  |
**500** | Clear failed |  -  |

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)

# **post_maintenance_reconcile**
> MaintenanceReconcileResponse post_maintenance_reconcile(maintenance_reconcile_request=maintenance_reconcile_request)

Reconcile orphaned vector records

### Example

* Bearer Authentication (bearerAuth):

```python
import aimee_kb
from aimee_kb.models.maintenance_reconcile_request import MaintenanceReconcileRequest
from aimee_kb.models.maintenance_reconcile_response import MaintenanceReconcileResponse
from aimee_kb.rest import ApiException
from pprint import pprint

# Defining the host is optional and defaults to http://127.0.0.1:8090/v1
# See configuration.py for a list of all supported configuration parameters.
configuration = aimee_kb.Configuration(
    host = "http://127.0.0.1:8090/v1"
)

# The client must configure the authentication and authorization parameters
# in accordance with the API server security policy.
# Examples for each auth method are provided below, use the example that
# satisfies your auth use case.

# Configure Bearer authorization: bearerAuth
configuration = aimee_kb.Configuration(
    access_token = os.environ["BEARER_TOKEN"]
)

# Enter a context with an instance of the API client
with aimee_kb.ApiClient(configuration) as api_client:
    # Create an instance of the API class
    api_instance = aimee_kb.DefaultApi(api_client)
    maintenance_reconcile_request = aimee_kb.MaintenanceReconcileRequest() # MaintenanceReconcileRequest |  (optional)

    try:
        # Reconcile orphaned vector records
        api_response = api_instance.post_maintenance_reconcile(maintenance_reconcile_request=maintenance_reconcile_request)
        print("The response of DefaultApi->post_maintenance_reconcile:\n")
        pprint(api_response)
    except Exception as e:
        print("Exception when calling DefaultApi->post_maintenance_reconcile: %s\n" % e)
```



### Parameters


Name | Type | Description  | Notes
------------- | ------------- | ------------- | -------------
 **maintenance_reconcile_request** | [**MaintenanceReconcileRequest**](MaintenanceReconcileRequest.md)|  | [optional] 

### Return type

[**MaintenanceReconcileResponse**](MaintenanceReconcileResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

 - **Content-Type**: application/json
 - **Accept**: application/json

### HTTP response details

| Status code | Description | Response headers |
|-------------|-------------|------------------|
**200** | Reconcile complete |  -  |
**401** | Unauthorized |  -  |
**500** | Reconcile failed |  -  |

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)

# **post_maintenance_repair**
> MaintenanceRepairResponse post_maintenance_repair(maintenance_repair_request)

Repair a project knowledge index

### Example

* Bearer Authentication (bearerAuth):

```python
import aimee_kb
from aimee_kb.models.maintenance_repair_request import MaintenanceRepairRequest
from aimee_kb.models.maintenance_repair_response import MaintenanceRepairResponse
from aimee_kb.rest import ApiException
from pprint import pprint

# Defining the host is optional and defaults to http://127.0.0.1:8090/v1
# See configuration.py for a list of all supported configuration parameters.
configuration = aimee_kb.Configuration(
    host = "http://127.0.0.1:8090/v1"
)

# The client must configure the authentication and authorization parameters
# in accordance with the API server security policy.
# Examples for each auth method are provided below, use the example that
# satisfies your auth use case.

# Configure Bearer authorization: bearerAuth
configuration = aimee_kb.Configuration(
    access_token = os.environ["BEARER_TOKEN"]
)

# Enter a context with an instance of the API client
with aimee_kb.ApiClient(configuration) as api_client:
    # Create an instance of the API class
    api_instance = aimee_kb.DefaultApi(api_client)
    maintenance_repair_request = aimee_kb.MaintenanceRepairRequest() # MaintenanceRepairRequest | 

    try:
        # Repair a project knowledge index
        api_response = api_instance.post_maintenance_repair(maintenance_repair_request)
        print("The response of DefaultApi->post_maintenance_repair:\n")
        pprint(api_response)
    except Exception as e:
        print("Exception when calling DefaultApi->post_maintenance_repair: %s\n" % e)
```



### Parameters


Name | Type | Description  | Notes
------------- | ------------- | ------------- | -------------
 **maintenance_repair_request** | [**MaintenanceRepairRequest**](MaintenanceRepairRequest.md)|  | 

### Return type

[**MaintenanceRepairResponse**](MaintenanceRepairResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

 - **Content-Type**: application/json
 - **Accept**: application/json

### HTTP response details

| Status code | Description | Response headers |
|-------------|-------------|------------------|
**200** | Repair complete |  -  |
**400** | Missing required parameters |  -  |
**401** | Unauthorized |  -  |
**503** | Knowledge or vector store unavailable |  -  |

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)

# **post_promote**
> post_promote(id)

Promote a release to active

### Example

* Bearer Authentication (bearerAuth):

```python
import aimee_kb
from aimee_kb.rest import ApiException
from pprint import pprint

# Defining the host is optional and defaults to http://127.0.0.1:8090/v1
# See configuration.py for a list of all supported configuration parameters.
configuration = aimee_kb.Configuration(
    host = "http://127.0.0.1:8090/v1"
)

# The client must configure the authentication and authorization parameters
# in accordance with the API server security policy.
# Examples for each auth method are provided below, use the example that
# satisfies your auth use case.

# Configure Bearer authorization: bearerAuth
configuration = aimee_kb.Configuration(
    access_token = os.environ["BEARER_TOKEN"]
)

# Enter a context with an instance of the API client
with aimee_kb.ApiClient(configuration) as api_client:
    # Create an instance of the API class
    api_instance = aimee_kb.DefaultApi(api_client)
    id = 56 # int | 

    try:
        # Promote a release to active
        api_instance.post_promote(id)
    except Exception as e:
        print("Exception when calling DefaultApi->post_promote: %s\n" % e)
```



### Parameters


Name | Type | Description  | Notes
------------- | ------------- | ------------- | -------------
 **id** | **int**|  | 

### Return type

void (empty response body)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

 - **Content-Type**: Not defined
 - **Accept**: Not defined

### HTTP response details

| Status code | Description | Response headers |
|-------------|-------------|------------------|
**200** | Release promoted |  -  |
**400** | Invalid id |  -  |
**401** | Unauthorized |  -  |
**409** | Promote failed |  -  |

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)

# **post_releases**
> CreateReleaseResponse post_releases(create_release_request)

Create a new corpus release

### Example

* Bearer Authentication (bearerAuth):

```python
import aimee_kb
from aimee_kb.models.create_release_request import CreateReleaseRequest
from aimee_kb.models.create_release_response import CreateReleaseResponse
from aimee_kb.rest import ApiException
from pprint import pprint

# Defining the host is optional and defaults to http://127.0.0.1:8090/v1
# See configuration.py for a list of all supported configuration parameters.
configuration = aimee_kb.Configuration(
    host = "http://127.0.0.1:8090/v1"
)

# The client must configure the authentication and authorization parameters
# in accordance with the API server security policy.
# Examples for each auth method are provided below, use the example that
# satisfies your auth use case.

# Configure Bearer authorization: bearerAuth
configuration = aimee_kb.Configuration(
    access_token = os.environ["BEARER_TOKEN"]
)

# Enter a context with an instance of the API client
with aimee_kb.ApiClient(configuration) as api_client:
    # Create an instance of the API class
    api_instance = aimee_kb.DefaultApi(api_client)
    create_release_request = aimee_kb.CreateReleaseRequest() # CreateReleaseRequest | 

    try:
        # Create a new corpus release
        api_response = api_instance.post_releases(create_release_request)
        print("The response of DefaultApi->post_releases:\n")
        pprint(api_response)
    except Exception as e:
        print("Exception when calling DefaultApi->post_releases: %s\n" % e)
```



### Parameters


Name | Type | Description  | Notes
------------- | ------------- | ------------- | -------------
 **create_release_request** | [**CreateReleaseRequest**](CreateReleaseRequest.md)|  | 

### Return type

[**CreateReleaseResponse**](CreateReleaseResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

 - **Content-Type**: application/json
 - **Accept**: application/json

### HTTP response details

| Status code | Description | Response headers |
|-------------|-------------|------------------|
**201** | Release created |  -  |
**400** | Missing name |  -  |
**401** | Unauthorized |  -  |
**409** | Name already exists |  -  |

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)

# **post_review_accept**
> post_review_accept(id, review_accept_request=review_accept_request)

Accept a staged document

### Example

* Bearer Authentication (bearerAuth):

```python
import aimee_kb
from aimee_kb.models.review_accept_request import ReviewAcceptRequest
from aimee_kb.rest import ApiException
from pprint import pprint

# Defining the host is optional and defaults to http://127.0.0.1:8090/v1
# See configuration.py for a list of all supported configuration parameters.
configuration = aimee_kb.Configuration(
    host = "http://127.0.0.1:8090/v1"
)

# The client must configure the authentication and authorization parameters
# in accordance with the API server security policy.
# Examples for each auth method are provided below, use the example that
# satisfies your auth use case.

# Configure Bearer authorization: bearerAuth
configuration = aimee_kb.Configuration(
    access_token = os.environ["BEARER_TOKEN"]
)

# Enter a context with an instance of the API client
with aimee_kb.ApiClient(configuration) as api_client:
    # Create an instance of the API class
    api_instance = aimee_kb.DefaultApi(api_client)
    id = 56 # int | 
    review_accept_request = aimee_kb.ReviewAcceptRequest() # ReviewAcceptRequest |  (optional)

    try:
        # Accept a staged document
        api_instance.post_review_accept(id, review_accept_request=review_accept_request)
    except Exception as e:
        print("Exception when calling DefaultApi->post_review_accept: %s\n" % e)
```



### Parameters


Name | Type | Description  | Notes
------------- | ------------- | ------------- | -------------
 **id** | **int**|  | 
 **review_accept_request** | [**ReviewAcceptRequest**](ReviewAcceptRequest.md)|  | [optional] 

### Return type

void (empty response body)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

 - **Content-Type**: application/json
 - **Accept**: Not defined

### HTTP response details

| Status code | Description | Response headers |
|-------------|-------------|------------------|
**200** | Document accepted |  -  |
**400** | Invalid id |  -  |
**401** | Unauthorized |  -  |
**404** | Document not found |  -  |

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)

# **post_review_reject**
> post_review_reject(id, review_reject_request)

Reject a staged document

### Example

* Bearer Authentication (bearerAuth):

```python
import aimee_kb
from aimee_kb.models.review_reject_request import ReviewRejectRequest
from aimee_kb.rest import ApiException
from pprint import pprint

# Defining the host is optional and defaults to http://127.0.0.1:8090/v1
# See configuration.py for a list of all supported configuration parameters.
configuration = aimee_kb.Configuration(
    host = "http://127.0.0.1:8090/v1"
)

# The client must configure the authentication and authorization parameters
# in accordance with the API server security policy.
# Examples for each auth method are provided below, use the example that
# satisfies your auth use case.

# Configure Bearer authorization: bearerAuth
configuration = aimee_kb.Configuration(
    access_token = os.environ["BEARER_TOKEN"]
)

# Enter a context with an instance of the API client
with aimee_kb.ApiClient(configuration) as api_client:
    # Create an instance of the API class
    api_instance = aimee_kb.DefaultApi(api_client)
    id = 56 # int | 
    review_reject_request = aimee_kb.ReviewRejectRequest() # ReviewRejectRequest | 

    try:
        # Reject a staged document
        api_instance.post_review_reject(id, review_reject_request)
    except Exception as e:
        print("Exception when calling DefaultApi->post_review_reject: %s\n" % e)
```



### Parameters


Name | Type | Description  | Notes
------------- | ------------- | ------------- | -------------
 **id** | **int**|  | 
 **review_reject_request** | [**ReviewRejectRequest**](ReviewRejectRequest.md)|  | 

### Return type

void (empty response body)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

 - **Content-Type**: application/json
 - **Accept**: Not defined

### HTTP response details

| Status code | Description | Response headers |
|-------------|-------------|------------------|
**200** | Document rejected |  -  |
**400** | Invalid id |  -  |
**401** | Unauthorized |  -  |
**404** | Document not found |  -  |

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)

# **post_rollback**
> post_rollback(id, rollback_request=rollback_request)

Roll back to a prior release

### Example

* Bearer Authentication (bearerAuth):

```python
import aimee_kb
from aimee_kb.models.rollback_request import RollbackRequest
from aimee_kb.rest import ApiException
from pprint import pprint

# Defining the host is optional and defaults to http://127.0.0.1:8090/v1
# See configuration.py for a list of all supported configuration parameters.
configuration = aimee_kb.Configuration(
    host = "http://127.0.0.1:8090/v1"
)

# The client must configure the authentication and authorization parameters
# in accordance with the API server security policy.
# Examples for each auth method are provided below, use the example that
# satisfies your auth use case.

# Configure Bearer authorization: bearerAuth
configuration = aimee_kb.Configuration(
    access_token = os.environ["BEARER_TOKEN"]
)

# Enter a context with an instance of the API client
with aimee_kb.ApiClient(configuration) as api_client:
    # Create an instance of the API class
    api_instance = aimee_kb.DefaultApi(api_client)
    id = 56 # int | 
    rollback_request = aimee_kb.RollbackRequest() # RollbackRequest |  (optional)

    try:
        # Roll back to a prior release
        api_instance.post_rollback(id, rollback_request=rollback_request)
    except Exception as e:
        print("Exception when calling DefaultApi->post_rollback: %s\n" % e)
```



### Parameters


Name | Type | Description  | Notes
------------- | ------------- | ------------- | -------------
 **id** | **int**|  | 
 **rollback_request** | [**RollbackRequest**](RollbackRequest.md)|  | [optional] 

### Return type

void (empty response body)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

 - **Content-Type**: application/json
 - **Accept**: Not defined

### HTTP response details

| Status code | Description | Response headers |
|-------------|-------------|------------------|
**200** | Rolled back |  -  |
**400** | Invalid id |  -  |
**401** | Unauthorized |  -  |
**409** | No prior release to roll back to |  -  |

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)

# **post_search**
> SearchResponse post_search(search_request)

Hybrid knowledge search

### Example

* Bearer Authentication (bearerAuth):

```python
import aimee_kb
from aimee_kb.models.search_request import SearchRequest
from aimee_kb.models.search_response import SearchResponse
from aimee_kb.rest import ApiException
from pprint import pprint

# Defining the host is optional and defaults to http://127.0.0.1:8090/v1
# See configuration.py for a list of all supported configuration parameters.
configuration = aimee_kb.Configuration(
    host = "http://127.0.0.1:8090/v1"
)

# The client must configure the authentication and authorization parameters
# in accordance with the API server security policy.
# Examples for each auth method are provided below, use the example that
# satisfies your auth use case.

# Configure Bearer authorization: bearerAuth
configuration = aimee_kb.Configuration(
    access_token = os.environ["BEARER_TOKEN"]
)

# Enter a context with an instance of the API client
with aimee_kb.ApiClient(configuration) as api_client:
    # Create an instance of the API class
    api_instance = aimee_kb.DefaultApi(api_client)
    search_request = aimee_kb.SearchRequest() # SearchRequest | 

    try:
        # Hybrid knowledge search
        api_response = api_instance.post_search(search_request)
        print("The response of DefaultApi->post_search:\n")
        pprint(api_response)
    except Exception as e:
        print("Exception when calling DefaultApi->post_search: %s\n" % e)
```



### Parameters


Name | Type | Description  | Notes
------------- | ------------- | ------------- | -------------
 **search_request** | [**SearchRequest**](SearchRequest.md)|  | 

### Return type

[**SearchResponse**](SearchResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

 - **Content-Type**: application/json
 - **Accept**: application/json

### HTTP response details

| Status code | Description | Response headers |
|-------------|-------------|------------------|
**200** | Search results |  -  |
**400** | Bad request (missing query) |  -  |
**401** | Unauthorized |  -  |

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)

