// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Wordprocessing.hpp"
#include "ExyokiOffice/Version.hpp"
#include "ExyokiOffice/Word/WordDocument.hpp"

#include <iostream>
#include <string>

int main()
{
    const auto editor = ExyokiOffice::Word::WordDocumentEditor::CreateNew();
    if (!editor || ExyokiOffice::GetVersion() != ExyokiOffice::Version::String)
        return 1;

    editor->AddParagraph("Installed ExyokiOffice " + std::string(ExyokiOffice::GetVersion()));
    std::cout << ExyokiOffice::GetVersion() << '\n';
    return 0;
}
