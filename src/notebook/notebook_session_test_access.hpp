// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "hieda/notebook/notebook_session.hpp"

namespace hieda::notebook {

class NotebookSessionTestAccess {
  public:
    static void rejectNextCommit(NotebookSession& session);
    static void setCurrentTimestamp(std::optional<BlockTimestamp> timestamp);
};

} // namespace hieda::notebook
