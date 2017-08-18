/*
* All or portions of this file Copyright (c) Amazon.com, Inc. or its affiliates or
* its licensors.
*
* For complete copyright and license terms please see the LICENSE at the root of this
* distribution (the "License"). All use of this software is governed by the License,
* or, if provided, by the license below or the license accompanying this file. Do not
* remove or modify any license notices. This file is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
*
*/
#include "stdafx.h"

#include "EditorCommon.h"

SerializeHelpers::SerializedEntryList& HierarchyClipboard::Serialize(HierarchyWidget* widget,
    QTreeWidgetItemRawPtrQList& selectedItems,
    HierarchyItemRawPtrList* optionalItemsToSerialize,
    SerializeHelpers::SerializedEntryList& entryList,
    bool isUndo)
{
    HierarchyItemRawPtrList itemsToSerialize;
    if (optionalItemsToSerialize)
    {
        // copy the list so we can sort it
        itemsToSerialize = *optionalItemsToSerialize;
    }
    else
    {
        SelectionHelpers::GetListOfTopLevelSelectedItems(widget,
            selectedItems,
            widget->invisibleRootItem(),
            itemsToSerialize);
    }

    // Sort the itemsToSerialize by order in the hierarchy, this is important for reliably restoring
    // them given that we maintain the order by remembering which item to insert before
    HierarchyHelpers::SortByHierarchyOrder(itemsToSerialize);

    // HierarchyItemRawPtrList -> SerializeHelpers::SerializedEntryList
    for (auto i : itemsToSerialize)
    {
        AZ::Entity* e = i->GetElement();
        AZ_Assert(e, "No entity found for item");

        // serialize this entity (and its descendants) to XML and get the set of prefab references
        // used by the serialized entities
        AZStd::unordered_set<AZ::Data::AssetId> referencedSliceAssets;
        AZStd::string xml = GetXml(widget, LyShine::EntityArray(1, e), referencedSliceAssets);
        AZ_Assert(!xml.empty(), "Failed to serialize");

        if (isUndo)
        {
            AZ::EntityId parentId;
            {
                HierarchyItem* parent = i->Parent();
                parentId = (parent ? parent->GetEntityId() : AZ::EntityId());
            }

            AZ::EntityId insertAboveThisId;
            {
                QTreeWidgetItem* parentItem = (i->QTreeWidgetItem::parent() ? i->QTreeWidgetItem::parent() : widget->invisibleRootItem());

                // +1 : Because the insertion point is the next sibling.
                QTreeWidgetItem* insertAboveThisBaseItem = parentItem->child(parentItem->indexOfChild(i) + 1);
                HierarchyItem* insertAboveThisItem = dynamic_cast<HierarchyItem*>(insertAboveThisBaseItem);

                insertAboveThisId = (insertAboveThisItem ? insertAboveThisItem->GetEntityId() : AZ::EntityId());
            }

            entryList.push_back(SerializeHelpers::SerializedEntry
                {
                    i->GetEntityId(),
                    parentId,
                    insertAboveThisId,
                    xml,
                    "",
                    referencedSliceAssets
                }
            );
        }
        else // isRedo.
        {
            AZ::EntityId id = i->GetEntityId();

            auto iter = std::find_if(entryList.begin(),
                    entryList.end(),
                    [ id ](const SerializeHelpers::SerializedEntry& entry)
                    {
                        return (entry.m_id == id);
                    });

            // This function should ALWAYS be called
            // with ( isUndo == true ) first.
            AZ_Assert(entryList.size() > 0, "Empty entry list");
            AZ_Assert(iter != entryList.end(), "Entity ID not found in entryList");

            iter->m_redoXml = xml;
        }
    }

    return entryList;
}

bool HierarchyClipboard::Unserialize(HierarchyWidget* widget,
    SerializeHelpers::SerializedEntryList& entryList,
    bool isUndo)
{
    if (!HierarchyHelpers::AllItemExists(widget, entryList))
    {
        // At least one item is missing.
        // Nothing to do.
        return false;
    }

    // Runtime-side: Replace element.
    for (auto && e : entryList)
    {
        HierarchyItem* item = dynamic_cast<HierarchyItem*>(HierarchyHelpers::ElementToItem(widget, e.m_id, false));
        item->ReplaceElement(isUndo ? e.m_undoXml : e.m_redoXml, e.m_referencedSliceAssets);
    }

    // Editor-side: Highlight.
    widget->clearSelection();
    HierarchyHelpers::SetSelectedItems(widget, &entryList);

    return true;
}

void HierarchyClipboard::CopySelectedItemsToClipboard(HierarchyWidget* widget,
    QTreeWidgetItemRawPtrQList& selectedItems)
{
    // selectedItems -> EntityArray.
    LyShine::EntityArray elements;
    {
        HierarchyItemRawPtrList itemsToSerialize;
        SelectionHelpers::GetListOfTopLevelSelectedItems(widget,
            selectedItems,
            widget->invisibleRootItem(),
            itemsToSerialize);

        for (auto i : itemsToSerialize)
        {
            elements.push_back(i->GetElement());
        }
    }

    // EntityArray -> XML.
    AZStd::unordered_set<AZ::Data::AssetId> referencedSliceAssets;    // returned from GetXML but not used in this case
    AZStd::string xml = GetXml(widget, elements, referencedSliceAssets);

    // XML -> Clipboard.
    if (!xml.empty())
    {
        IEditor* pEditor = GetIEditor();
        AZ_Assert(pEditor, "Failed to get IEditor");

        QMimeData* mimeData = pEditor->CreateQMimeData();
        {
            // Concatenate all the data we need into a single QByteArray.
            QByteArray data(xml.c_str(), xml.size());
            mimeData->setData(UICANVASEDITOR_MIMETYPE, data);
        }

        QApplication::clipboard()->setMimeData(mimeData);
    }
}

void HierarchyClipboard::CreateElementsFromClipboard(HierarchyWidget* widget,
    QTreeWidgetItemRawPtrQList& selectedItems,
    bool createAsChildOfSelection)
{
    if (!ClipboardContainsOurDataType())
    {
        // Nothing to do.
        return;
    }

    const QMimeData* mimeData = QApplication::clipboard()->mimeData();

    QByteArray data = mimeData->data(UICANVASEDITOR_MIMETYPE);
    char* rawData = data.data();

    // Extract all the data we need from the source QByteArray.
    char* xml = static_cast<char*>(rawData);

    CommandHierarchyItemCreateFromData::Push(widget->GetEditorWindow()->GetActiveStack(),
        widget,
        selectedItems,
        createAsChildOfSelection,
        [ widget, xml ](HierarchyItem* parent,
                                                  LyShine::EntityArray& listOfNewlyCreatedTopLevelElements)
        {
            SerializeHelpers::RestoreSerializedElements(widget->GetEditorWindow()->GetCanvas(),
                (parent ? parent->GetElement() : nullptr),
                nullptr,
                widget->GetEditorWindow()->GetEntityContext(),
                xml,
                true,
                &listOfNewlyCreatedTopLevelElements);
        },
        "Paste");
}

AZStd::string HierarchyClipboard::GetXml(HierarchyWidget* widget,
    const LyShine::EntityArray& elements,
    AZStd::unordered_set<AZ::Data::AssetId>& referencedSliceAssets)
{
    if (elements.empty())
    {
        // Nothing to do.
        return "";
    }

    AZ::EntityId canvasEntityId = widget->GetEditorWindow()->GetCanvas();
    AZ::SliceComponent* rootSlice = widget->GetEditorWindow()->GetSliceManager()->GetRootSlice();
    AZStd::string result = SerializeHelpers::SaveElementsToXmlString(elements, rootSlice, referencedSliceAssets);

    return result;
}

AZStd::string HierarchyClipboard::GetXmlForDiff(AZ::EntityId canvasEntityId)
{
    AZStd::string xmlString;
    EBUS_EVENT_ID_RESULT(xmlString, canvasEntityId, UiCanvasBus, SaveToXmlString);
    return xmlString;
}