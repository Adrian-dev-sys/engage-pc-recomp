// The eShop pages (nn::ec::ShowShop*) and the add-on content error (nn::aoc::ShowAddOnContentLostError) are library
// applets, which this port does not run: CreateLibraryApplet would fault. Each returns at once, as when the player
// closes the shop without buying (the game's DLC menu entry calls ShowShopAddOnContentList).
#include "host.h"
#include "platform/platform.h"

static void ov_shop(Cpu *c) {
    static volatile int32_t logged;
    if (!plat_xchg32(&logged, 1)) rt_log("ec: eShop page requested; this port has no eShop, so it closes at once");
    ret_x(c, 0);
}

HOST_OVERRIDE("_ZN2nn2ec19ShowShopProductListEPKNS0_5NsUidEiPKc", ov_shop);
HOST_OVERRIDE("_ZN2nn2ec19ShowShopProductListEPKNS0_5NsUidEiPKcRKNS_7account10UserHandleE", ov_shop);
HOST_OVERRIDE("_ZN2nn2ec22ShowShopEnterCodeSceneERKNS_7account10UserHandleE", ov_shop);
HOST_OVERRIDE("_ZN2nn2ec22ShowShopEnterCodeSceneEv", ov_shop);
HOST_OVERRIDE("_ZN2nn2ec22ShowShopProductDetailsENS0_5NsUidE", ov_shop);
HOST_OVERRIDE("_ZN2nn2ec22ShowShopProductDetailsENS0_5NsUidERKNS_7account10UserHandleE", ov_shop);
HOST_OVERRIDE("_ZN2nn2ec24ShowShopAddOnContentListENS_13ApplicationIdE", ov_shop);
HOST_OVERRIDE("_ZN2nn2ec24ShowShopAddOnContentListENS_13ApplicationIdERKNS_7account10UserHandleE", ov_shop);
HOST_OVERRIDE("_ZN2nn2ec24ShowShopSubscriptionListENS_13ApplicationIdE", ov_shop);
HOST_OVERRIDE("_ZN2nn2ec24ShowShopSubscriptionListENS_13ApplicationIdERKNS0_8CourseIdE", ov_shop);
HOST_OVERRIDE("_ZN2nn2ec24ShowShopSubscriptionListENS_13ApplicationIdERKNS0_8CourseIdERKNS_7account10UserHandleE", ov_shop);
HOST_OVERRIDE("_ZN2nn2ec24ShowShopSubscriptionListENS_13ApplicationIdERKNS_7account10UserHandleE", ov_shop);
HOST_OVERRIDE("_ZN2nn2ec26ShowShopConsumableItemListENS_13ApplicationIdE", ov_shop);
HOST_OVERRIDE("_ZN2nn2ec26ShowShopConsumableItemListENS_13ApplicationIdERKNS_7account10UserHandleE", ov_shop);
HOST_OVERRIDE("_ZN2nn2ec28ShowShopConsumableItemDetailENS_13ApplicationIdERKNS0_12ConsumableIdENS0_5NsUidE", ov_shop);
HOST_OVERRIDE("_ZN2nn2ec28ShowShopConsumableItemDetailENS_13ApplicationIdERKNS0_12ConsumableIdENS0_5NsUidERKNS_7account10UserHandleE", ov_shop);
HOST_OVERRIDE("_ZN2nn2ec30ShowShopApplicationInformationENS_13ApplicationIdE", ov_shop);
HOST_OVERRIDE("_ZN2nn2ec30ShowShopApplicationInformationENS_13ApplicationIdERKNS_7account10UserHandleE", ov_shop);
HOST_OVERRIDE("_ZN2nn3aoc25ShowAddOnContentLostErrorEPKii", ov_shop);
