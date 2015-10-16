/**
 * Copyright (c) 2015 Eugene Lazin <4lazin@gmail.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once
#include <string>
#include <memory>
#include <tuple>

#include <microhttpd.h>

#include "logger.h"
#include "akumuli.h"
#include "server.h"

namespace Akumuli {
namespace Http {

struct AccessControlList {};  // TODO: implement ACL

struct HttpServer : std::enable_shared_from_this<HttpServer>, Server
{
    AccessControlList                       acl_;
    std::shared_ptr<ReadOperationBuilder>   proc_;
    unsigned short                          port_;
    MHD_Daemon                             *daemon_;

    HttpServer(unsigned short port, std::shared_ptr<ReadOperationBuilder> qproc);
    HttpServer(unsigned short port, std::shared_ptr<ReadOperationBuilder> qproc, AccessControlList const& acl);

    virtual void start(SignalHandler* handler, int id);
    void stop();
};

}
}

