// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <http/server.h>

error_t handleRoot(HttpServerRequest* request, void* context);
error_t handleFileBrowser(HttpServerRequest* request, void* context);
error_t handleFsGet(HttpServerRequest* request, void* context);
error_t handleFsPost(HttpServerRequest* request, void* context);
error_t handleAdminPost(HttpServerRequest* request, void* context);
error_t handleApiGet(HttpServerRequest* request, void* context);
error_t handleApiPost(HttpServerRequest* request, void* context);
error_t handleApiPut(HttpServerRequest* request, void* context);
error_t handleAssets(HttpServerRequest* request, void* context);
