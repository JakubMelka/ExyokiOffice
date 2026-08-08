// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

namespace ExyokiOffice::Word
{
class WordDocumentEditor;
}

namespace ExyokiOffice::Excel
{
class ExcelDocumentEditor;
}

namespace ExyokiOffice::PowerPoint
{
class PowerPointDocumentEditor;
}

/**
 * @file
 * @brief Names the three family editors for the Tools headers that take one.
 *
 * Most entry points in ExyokiOffice::Tools come in two shapes: one that takes a
 * path and opens the document itself, and one that takes a document already
 * open in its family editor. The second shape only ever binds a reference, so
 * the headers declaring it do not need the editor definitions — and those three
 * headers are among the largest in the library, which is reason enough not to
 * pull them into every translation unit that wants to call Stat().
 *
 * A caller passing an editor necessarily includes its header already.
 */
