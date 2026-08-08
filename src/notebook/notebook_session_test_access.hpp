// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "hieda/notebook/notebook_session.hpp"

namespace hieda::notebook {

class NotebookSessionTestAccess {
  public:
    static void rejectNextCommit(NotebookSession& session);
};

} // namespace hieda::notebook
