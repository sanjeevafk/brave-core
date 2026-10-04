// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

// On desktop, discarding a tab replaces its WebContents. These tests check
// that BraveTabFeatures members keep working on the new contents.

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/test/scoped_feature_list.h"
#include "brave/components/ai_chat/core/common/buildflags/buildflags.h"
#include "brave/components/psst/buildflags/buildflags.h"
#include "chrome/browser/resource_coordinator/lifecycle_unit_state.mojom.h"
#include "chrome/browser/resource_coordinator/tab_lifecycle_unit_external.h"
#include "chrome/browser/resource_coordinator/tab_lifecycle_unit_source.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/ui_test_utils.h"
#include "components/tabs/public/tab_interface.h"
#include "content/public/browser/web_contents.h"
#include "content/public/test/browser_test.h"
#include "content/public/test/browser_test_utils.h"
#include "content/public/test/content_mock_cert_verifier.h"
#include "content/public/test/test_utils.h"
#include "net/dns/mock_host_resolver.h"
#include "net/test/embedded_test_server/embedded_test_server.h"
#include "ui/base/window_open_disposition.h"
#include "url/gurl.h"

#if BUILDFLAG(ENABLE_AI_CHAT)
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/test/run_until.h"
#include "base/threading/thread_restrictions.h"
#include "brave/browser/ai_chat/tab_tracker_service_factory.h"
#include "brave/components/ai_chat/core/browser/tab_tracker_service.h"
#include "brave/components/ai_chat/core/common/mojom/tab_tracker.mojom.h"
#include "brave/components/web_mcp/core/browser/web_mcp_rule_registry.h"
#include "chrome/browser/profiles/profile.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "third_party/blink/public/common/features.h"
#endif

#if BUILDFLAG(ENABLE_PSST)
#include "base/functional/callback_helpers.h"
#include "brave/browser/psst/psst_ui_desktop_presenter.h"
#include "brave/browser/ui/tabs/public/brave_tab_features.h"
#include "brave/browser/ui/views/page_action/psst_action_controller.h"
#include "brave/components/psst/core/common/features.h"
#include "components/infobars/content/content_infobar_manager.h"
#include "components/infobars/core/infobar.h"
#include "components/infobars/core/infobar_delegate.h"
#endif

namespace {

#if BUILDFLAG(ENABLE_AI_CHAT)
// Keeps the latest tab data reported by the TabTrackerService.
class TestTabDataObserver : public ai_chat::mojom::TabDataObserver {
 public:
  mojo::PendingRemote<ai_chat::mojom::TabDataObserver> BindAndPassRemote() {
    return receiver_.BindNewPipeAndPassRemote();
  }

  bool HasTab(int32_t id, const GURL& url, const std::string& title) const {
    for (const auto& tab : tabs_) {
      if (tab->id == id && tab->url == url && tab->title == title) {
        return true;
      }
    }
    return false;
  }

  // ai_chat::mojom::TabDataObserver:
  void TabDataChanged(std::vector<ai_chat::mojom::TabDataPtr> tabs) override {
    tabs_ = std::move(tabs);
  }

 private:
  std::vector<ai_chat::mojom::TabDataPtr> tabs_;
  mojo::Receiver<ai_chat::mojom::TabDataObserver> receiver_{this};
};
#endif

}  // namespace

class BraveTabFeaturesDiscardBrowserTest : public InProcessBrowserTest {
 public:
  BraveTabFeaturesDiscardBrowserTest() {
    std::vector<base::test::FeatureRef> enabled_features;
#if BUILDFLAG(ENABLE_AI_CHAT)
    enabled_features.push_back(blink::features::kWebMCP);
#endif
#if BUILDFLAG(ENABLE_PSST)
    enabled_features.push_back(psst::features::kEnablePsst);
#endif
    scoped_feature_list_.InitWithFeatures(enabled_features, {});
  }

  void SetUpCommandLine(base::CommandLine* command_line) override {
    InProcessBrowserTest::SetUpCommandLine(command_line);
    mock_cert_verifier_.SetUpCommandLine(command_line);
#if BUILDFLAG(ENABLE_AI_CHAT)
    // document.modelContext is an experimental runtime feature.
    command_line->AppendSwitchASCII("enable-blink-features", "WebMCP");
#endif
  }

  void SetUpInProcessBrowserTestFixture() override {
    InProcessBrowserTest::SetUpInProcessBrowserTestFixture();
    mock_cert_verifier_.SetUpInProcessBrowserTestFixture();
  }

  void TearDownInProcessBrowserTestFixture() override {
    mock_cert_verifier_.TearDownInProcessBrowserTestFixture();
    InProcessBrowserTest::TearDownInProcessBrowserTestFixture();
  }

  void SetUpOnMainThread() override {
    InProcessBrowserTest::SetUpOnMainThread();
    mock_cert_verifier_.mock_cert_verifier()->set_default_result(net::OK);
    host_resolver()->AddRule("*", "127.0.0.1");
    https_server_.ServeFilesFromSourceDirectory(GetChromeTestDataDir());
    ASSERT_TRUE(https_server_.Start());
  }

 protected:
  GURL GetURL(std::string_view path) {
    return https_server_.GetURL("a.com", path);
  }

  // Opens `url` in a new background tab and returns it.
  tabs::TabInterface* OpenBackgroundTab(const GURL& url) {
    EXPECT_TRUE(ui_test_utils::NavigateToURLWithDisposition(
        browser(), url, WindowOpenDisposition::NEW_BACKGROUND_TAB,
        ui_test_utils::BROWSER_TEST_WAIT_FOR_LOAD_STOP));
    TabStripModel* tab_strip_model = browser()->GetTabStripModel();
    return tab_strip_model->GetTabAtIndex(tab_strip_model->count() - 1);
  }

  // Discards the background `tab`, which replaces its WebContents, then
  // activates it so that it reloads into the new contents.
  void DiscardAndReload(tabs::TabInterface* tab) {
    content::WebContents* old_contents = tab->GetContents();
    content::WebContentsDestroyedWatcher destroyed_watcher(old_contents);
    ASSERT_TRUE(
        resource_coordinator::TabLifecycleUnitSource::
            GetTabLifecycleUnitExternal(old_contents)
                ->DiscardTab(mojom::LifecycleUnitDiscardReason::URGENT));
    destroyed_watcher.Wait();

    TabStripModel* tab_strip_model = browser()->GetTabStripModel();
    tab_strip_model->ActivateTabAt(tab_strip_model->GetIndexOfTab(tab));
    ASSERT_TRUE(content::WaitForLoadStop(tab->GetContents()));
  }

 private:
  base::test::ScopedFeatureList scoped_feature_list_;
  content::ContentMockCertVerifier mock_cert_verifier_;
  net::EmbeddedTestServer https_server_{net::EmbeddedTestServer::TYPE_HTTPS};
};

#if BUILDFLAG(ENABLE_AI_CHAT)
IN_PROC_BROWSER_TEST_F(BraveTabFeaturesDiscardBrowserTest,
                       TabTrackerFollowsDiscardedTab) {
  TestTabDataObserver observer;
  ai_chat::TabTrackerServiceFactory::GetForBrowserContext(
      browser()->GetProfile())
      ->AddObserver(observer.BindAndPassRemote());

  tabs::TabInterface* tab = OpenBackgroundTab(GetURL("/title2.html"));
  DiscardAndReload(tab);

  const GURL url = GetURL("/title3.html");
  ASSERT_TRUE(content::NavigateToURL(tab->GetContents(), url));
  EXPECT_TRUE(base::test::RunUntil([&] {
    return observer.HasTab(tab->GetHandle().raw_value(), url,
                           "Title Of More Awesomeness");
  }));
}

IN_PROC_BROWSER_TEST_F(BraveTabFeaturesDiscardBrowserTest,
                       WebMcpToolInjectedIntoDiscardedTab) {
  base::ScopedTempDir component_dir;
  {
    base::ScopedAllowBlockingForTesting allow_blocking;
    ASSERT_TRUE(component_dir.CreateUniqueTempDir());
    const base::FilePath scripts_dir =
        component_dir.GetPath().AppendASCII("scripts");
    ASSERT_TRUE(base::CreateDirectory(scripts_dir));
    // The port is part of the URL spec, hence the wildcard after the host.
    ASSERT_TRUE(base::WriteFile(scripts_dir.AppendASCII("tool.js"),
                                "// ==WebMCP==\n"
                                "// @name discard_test_tool\n"
                                "// @match https://a.com*\n"
                                "// @description Test tool.\n"
                                "// ==/WebMCP==\n"
                                "return 'ok';\n"));
  }
  auto* registry = web_mcp::WebMcpRuleRegistry::GetInstance();
  registry->LoadRules(component_dir.GetPath());
  ASSERT_TRUE(base::test::RunUntil([&] {
    for (const auto& rule : registry->rules()) {
      if (rule.tool_name == "discard_test_tool") {
        return true;
      }
    }
    return false;
  }));

  tabs::TabInterface* tab = OpenBackgroundTab(GetURL("/title2.html"));
  DiscardAndReload(tab);

  // Waits until the injected script has registered the tool.
  EXPECT_EQ(true, content::EvalJs(tab->GetContents(), R"JS(
    (async () => {
      while (!(await document.modelContext.getTools())
                 .some(tool => tool.name === 'discard_test_tool')) {
        await new Promise(resolve => setTimeout(resolve, 50));
      }
      return true;
    })()
  )JS"));
}
#endif  // BUILDFLAG(ENABLE_AI_CHAT)

#if BUILDFLAG(ENABLE_PSST)
IN_PROC_BROWSER_TEST_F(BraveTabFeaturesDiscardBrowserTest,
                       PsstInfoBarShownInDiscardedTab) {
  tabs::TabInterface* tab = OpenBackgroundTab(GetURL("/title2.html"));
  auto* psst_action_controller =
      tabs::BraveTabFeatures::FromTabFeatures(tab->GetTabFeatures())
          ->psst_page_action_controller();
  ASSERT_TRUE(psst_action_controller);
  psst::PsstUiDesktopPresenter presenter(*tab,
                                         psst_action_controller->AsWeakPtr());

  DiscardAndReload(tab);

  presenter.ShowInfoBar(base::DoNothing());
  auto* infobar_manager =
      infobars::ContentInfoBarManager::FromWebContents(tab->GetContents());
  ASSERT_TRUE(infobar_manager);
  ASSERT_EQ(1u, infobar_manager->infobars().size());
  EXPECT_EQ(infobars::InfoBarDelegate::BRAVE_PSST_INFOBAR_DELEGATE,
            infobar_manager->infobars()[0]->GetIdentifier());
}
#endif  // BUILDFLAG(ENABLE_PSST)
