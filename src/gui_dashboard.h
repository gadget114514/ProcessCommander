#pragma once

#include "process_manager.h"
#include "ipc_server.h"

namespace pc {

int RunGuiDashboard(ProcessManager& pm, IpcServer& ipc);

} // namespace pc
