#include "Entities/Creature/CreatureData.h"
#include "gtest/gtest.h"

TEST(VendorItemSourceTest, RawSlotIsIndependentOfLoadedVectorIndex)
{
    VendorItemData data;
    data.AddItem(100, 0, 0, 0, VendorItemSourceKind::NpcVendorRow, 9);
    data.AddItem(101, 0, 0, 0, VendorItemSourceKind::NpcVendorRow, 37);

    ASSERT_EQ(data.GetItemCount(), 2);
    EXPECT_EQ(data.GetItem(0)->item, 100);
    EXPECT_EQ(data.GetItem(0)->sourceSlot, 9);
    EXPECT_EQ(data.GetItem(1)->item, 101);
    EXPECT_EQ(data.GetItem(1)->sourceSlot, 37);

    VendorItemSourceLookup const match = data.FindNpcVendorRow(37, 101);
    ASSERT_EQ(match.matchCount, 1);
    ASSERT_NE(match.item, nullptr);
    EXPECT_EQ(match.item->item, 101);

    VendorItemSourceLookup const wrongSlot = data.FindNpcVendorRow(1, 101);
    EXPECT_EQ(wrongSlot.matchCount, 0);
    EXPECT_EQ(wrongSlot.item, nullptr);
    VendorItemSourceLookup const wrongItem = data.FindNpcVendorRow(37, 100);
    EXPECT_EQ(wrongItem.matchCount, 0);
    EXPECT_EQ(wrongItem.item, nullptr);
    data.Clear();
}

TEST(VendorItemSourceTest, DuplicateNaturalKeyIsAmbiguous)
{
    VendorItemData data;
    data.AddItem(200, 0, 0, 0, VendorItemSourceKind::NpcVendorRow, 4);
    data.AddItem(200, 1, 30, 2, VendorItemSourceKind::NpcVendorRow, 4);

    VendorItemSourceLookup const match = data.FindNpcVendorRow(4, 200);
    EXPECT_EQ(match.matchCount, 2);
    EXPECT_EQ(match.item, nullptr);
    data.Clear();
}

TEST(VendorItemSourceTest, ReferenceAndRuntimeRowsCannotMasqueradeAsTableRows)
{
    VendorItemData data;
    data.AddItem(300, 0, 0, 0, VendorItemSourceKind::VendorReferenceExpansion);
    data.AddItem(300, 0, 0, 0, VendorItemSourceKind::Runtime);

    VendorItemSourceLookup const match = data.FindNpcVendorRow(6, 300);
    EXPECT_EQ(match.matchCount, 0);
    EXPECT_EQ(match.item, nullptr);
    data.Clear();
}

TEST(VendorItemSourceTest, MissingSourceKeyHasNoLoadedMatch)
{
    VendorItemData data;
    data.AddItem(400, 0, 0, 0, VendorItemSourceKind::NpcVendorRow, 2);

    VendorItemSourceLookup const filtered = data.FindNpcVendorRow(12, 401);
    EXPECT_EQ(filtered.matchCount, 0);
    EXPECT_EQ(filtered.item, nullptr);
    data.Clear();
}
