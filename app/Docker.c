#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>
#include "cJSON.h"

#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

int WaitForProcessReturnCode(int pid) {
    for (;;) {
        int child_status;
        waitpid(pid, &child_status, 0);
        if (WIFEXITED(child_status)) {
            return WEXITSTATUS(child_status);
        } else if (WIFSIGNALED(child_status)) {
            return -1;
        }
        /* WIFSTOPPED & WIFCONTINUED do not interest us */
    }

    /* Unreachable */
    return 0;
}

static const char* const template = "/tmp/tmp.XXXXXX";

const char* CreateTempFile(int* open_fd) {
    char* file_template = strdup(template);

    if (!file_template) {
        return NULL;
    }

    int fd = mkstemp(file_template);

    if (fd == -1) {
        free(file_template);
        return NULL;
    }

    if (open_fd) {
        *open_fd = fd;
    } else {
        close(fd);
    }

    return file_template;
}

struct Buffer {
    char* buffer;
    size_t size;
};

static int AppendToBuffer(struct Buffer* buffer, const char* data, size_t size) {
    if (size == 0) {
        return 0;
    }

    char* new_buffer = buffer->buffer ? realloc(buffer->buffer, buffer->size + size) : malloc(size);

    if (!new_buffer) {
        return 1;
    }

    buffer->buffer = new_buffer;
    memcpy(buffer->buffer + buffer->size, data, size);
    buffer->size += size;

    return 0;
}

static size_t WriteToBuffer(char* ptr, size_t size, size_t nmemb, void* userdata) {
    struct Buffer* buffer = userdata;
    const size_t actual_size = size * nmemb;

    if (AppendToBuffer(buffer, ptr, actual_size) != 0) {
        return 0;
    }

    return actual_size;
}

static size_t WriteToFile(char* ptr, size_t size, size_t nmemb, void* userdata) {
    const int fd = *(int*)userdata;
    const size_t actual_size = size * nmemb;
    return (size_t)write(fd, ptr, actual_size);
}

static const char* CreateAuthenticationTokenUrl(const char* repository, const char* image) {
    static const char* const kTokenUrlFormatStr =
        "https://auth.docker.io/token?service=registry.docker.io&scope=repository:%s/%s:pull";

    static const size_t kTokenUrlFormatSpecifiers = 2;

    char* url = malloc(strlen(kTokenUrlFormatStr) - 2 * kTokenUrlFormatSpecifiers + strlen(repository) + strlen(image) + 1);

    if (url) {
        sprintf(url, kTokenUrlFormatStr, repository, image);
    }

    return url;
}

static const char* GetAuthenticationToken(CURL* curl, const char* repository, const char* image) {
    struct Buffer buffer = {0};

    const char* url = CreateAuthenticationTokenUrl(repository, image);

    curl_easy_reset(curl);
    curl_easy_setopt(curl, CURLOPT_VERBOSE, 0);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &WriteToBuffer);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer);
    curl_easy_setopt(curl, CURLOPT_URL, url);

    if (curl_easy_perform(curl) != CURLE_OK) {
        return NULL;
    }

    cJSON* json_response = cJSON_ParseWithLength(buffer.buffer, buffer.size);
    if (!json_response) {
        return NULL;
    }

    cJSON* json_token = cJSON_GetObjectItemCaseSensitive(json_response, "token");

    if (!cJSON_IsString(json_token) || !json_token->valuestring) {
        return NULL;
    }

    const char* token = strdup(json_token->valuestring);

    free((void*) url);
    free(buffer.buffer);
    cJSON_Delete(json_response);

    return token;
}

static const char* CreateImageManifestUrl(const char* repository, const char* image, const char* tag) {
    static const char* const kManifestUrlFormatStr = "https://registry.hub.docker.com/v2/%s/%s/manifests/%s";
    static const size_t kManifestUrlFormatSpecifiers = 3;

    char* manifest_url = malloc(
        strlen(kManifestUrlFormatStr) - 2 * kManifestUrlFormatSpecifiers + strlen(repository) + strlen(image) +
        strlen(tag) + 1);

    if (manifest_url) {
        sprintf(manifest_url, kManifestUrlFormatStr, repository, image, tag);
    }

    return manifest_url;
}

static const char* CreateAuhorizationHeader(const char* token) {
    static const char* const kAuthHeaderFormatStr = "Authorization: Bearer %s";
    static const size_t kAuthHeaderFormatSpecifiers = 1;

    char* auth_header = malloc(strlen(kAuthHeaderFormatStr) - 2 * kAuthHeaderFormatSpecifiers + strlen(token) + 1);

    if (auth_header) {
        sprintf(auth_header, kAuthHeaderFormatStr, token);
    }

    return auth_header;
}

static const char* CreateImageLayerUrl(const char* repository, const char* image, const char* digest) {
    static const char* const kLayerUrlFormatStr = "https://registry.hub.docker.com/v2/%s/%s/blobs/%s";
    static const size_t kLayerUrlFormatSpecifiers = 3;
    char* layer_url = malloc(
        strlen(kLayerUrlFormatStr) - 2 * kLayerUrlFormatSpecifiers + strlen(repository) + strlen(image) +
        strlen(digest) + 1);

    if (layer_url) {
        sprintf(layer_url, kLayerUrlFormatStr, repository, image, digest);
    }

    return layer_url;
}

static int PullImageLayer(CURL* curl, const char* token, const char* repository, const char* image, const char* digest, const char* dir) {
    int fd;
    struct curl_slist* headers = NULL;
    const char* temp_file_name = CreateTempFile(&fd);

    if (!temp_file_name) {
        return 1;
    }

    curl_easy_reset(curl);
    curl_easy_setopt(curl, CURLOPT_VERBOSE, 0);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &WriteToFile);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &fd);

    const char* layer_url = CreateImageLayerUrl(repository, image, digest);
    curl_easy_setopt(curl, CURLOPT_URL, layer_url);
    free((void*) layer_url);

    const char* auth_header = CreateAuhorizationHeader(token);
    headers = curl_slist_append(headers, auth_header);
    free((void*) auth_header);

    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    if (curl_easy_perform(curl) != CURLE_OK) {
        fputs("Error downloading image layer", stderr);
        return 1;
    }

    curl_slist_free_all(headers);
    lseek(fd, 0, SEEK_SET);
    pid_t untar_pid = fork();

    if (untar_pid == -1) {
        return 1;
    }

    if (untar_pid == 0) {
        dup2(fd, STDIN_FILENO);
        close(fd);
        execlp("tar", "tar", "-C", dir, "-xzf", "-", NULL);
        exit(1);
    }

    if (WaitForProcessReturnCode(untar_pid) != 0) {
        fputs("Error unarchiving image layer", stderr);
        return 1;
    }

    close(fd);
    unlink(temp_file_name);

    return 0;
}

static int PullImageLayers(CURL* curl, const char* token, const char* repository, const char* image, const char* tag, const char* dir) {
    const char* manifest_url;
    const char* auth_header;
    struct curl_slist* headers = NULL;
    struct Buffer buffer = {0};
    cJSON* json_response;
    cJSON* json_layers;
    cJSON* json_layer;

    curl_easy_setopt(curl, CURLOPT_VERBOSE, 0);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &WriteToBuffer);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer);

    manifest_url = CreateImageManifestUrl(repository, image, tag);
    curl_easy_setopt(curl, CURLOPT_URL, manifest_url);
    free((void*) manifest_url);
    headers = curl_slist_append(headers, "Accept: application/vnd.docker.distribution.manifest.v2+json");
    auth_header = CreateAuhorizationHeader(token);
    headers = curl_slist_append(headers, auth_header);
    free((void*) auth_header);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    if (curl_easy_perform(curl) != CURLE_OK) {
        fputs("Error getting image manifest.", stderr);
        return 1;
    }

    curl_slist_free_all(headers);
    json_response = cJSON_ParseWithLength(buffer.buffer, buffer.size);
    free(buffer.buffer);

    if (!json_response) {
        return 1;
    }

    json_layers = cJSON_GetObjectItemCaseSensitive(json_response, "layers");

    cJSON_ArrayForEach(json_layer, json_layers) {
        cJSON* json_digest = cJSON_GetObjectItemCaseSensitive(json_layer, "digest");

        if (!cJSON_IsString(json_digest) || !json_digest->valuestring) {
            return 1;
        }

        if (PullImageLayer(curl, token, repository, image, json_digest->valuestring, dir) != 0) {
            fprintf(stderr, "Error pulling image layer %s.", json_digest->valuestring);
            return 1;
        }
    }

    return 0;
}

int PullDockerImage(const char* full_image_name, const char* dir) {
    static const char* const kDefaultRepositoryName = "library";
    static const char* const kDefaultTag = "latest";

    const char* repository_last = strchr(full_image_name, '/');
    const char* repository = repository_last ? strndup(full_image_name, repository_last - full_image_name) : kDefaultRepositoryName;
    const char* tag_prev_first = strrchr(full_image_name, ':');
    const char* tag = tag_prev_first ? tag_prev_first + 1 : kDefaultTag;
    const char* image_first = repository_last ? repository_last + 1 : full_image_name;
    char* image = strndup(image_first, tag_prev_first ? tag_prev_first - image_first : strlen(image_first));

    curl_global_init(CURL_GLOBAL_ALL);

    CURL* curl = curl_easy_init();
    if (!curl) {
        return 1;
    }

    const char* token = GetAuthenticationToken(curl, repository, image);
    if (!token) {
        fputs("Error aquiring authentication token.", stderr);
        return 1;
    }

    if (PullImageLayers(curl, token, repository, image, tag, dir) != 0) {
        return 1;
    }

    if (repository != kDefaultRepositoryName) {
        free((void*) repository);
    }

    free(image);
    curl_easy_cleanup(curl);
    curl_global_cleanup();
    return 0;
}
