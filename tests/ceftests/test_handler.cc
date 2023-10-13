// Copyright (c) 2013 The Chromium Embedded Framework Authors. All rights
// reserved. Use of this source code is governed by a BSD-style license that
// can be found in the LICENSE file.

#include "tests/ceftests/test_handler.h"

#include <sstream>

#include "include/base/cef_callback.h"
#include "include/base/cef_logging.h"
#include "include/cef_command_line.h"
#include "include/cef_stream.h"
#include "include/views/cef_browser_view.h"
#include "include/views/cef_window.h"
#include "include/wrapper/cef_closure_task.h"
#include "include/wrapper/cef_stream_resource_handler.h"
#include "tests/ceftests/test_request.h"
#include "tests/ceftests/test_util.h"
#include "tests/shared/common/client_switches.h"

// Set to 1 to enable verbose debugging info logging.
#define VERBOSE_DEBUGGING 0

namespace {

bool UseViews() {
  static bool use_views = []() {
    return CefCommandLine::GetGlobalCommandLine()->HasSwitch(
        client::switches::kUseViews);
  }();
  return use_views;
}

// Delegate implementation for the CefWindow that will host the Views-based
// browser.
class TestWindowDelegate : public CefWindowDelegate {
 public:
  // Create a new top-level Window hosting |browser_view|.
  static void CreateBrowserWindow(TestHandler* handler,
                                  CefRefPtr<CefBrowserView> browser_view,
                                  const std::string& title) {
    CefWindow::CreateTopLevelWindow(new TestWindowDelegate(
        handler, browser_view, "CefUnitTestViews " + title));
  }

  // CefWindowDelegate methods:

  void OnWindowCreated(CefRefPtr<CefWindow> window) override {
    // Add the browser view and show the window.
    window->CenterWindow(CefSize(800, 600));
    window->SetTitle(title_);
    window->AddChildView(browser_view_);
    window->Show();

    // With Chrome runtime, the Browser is not created until after the
    // BrowserView is assigned to the Window.
    browser_id_ = browser_view_->GetBrowser()->GetIdentifier();
    handler_->OnWindowCreated(browser_id_);
  }

  void OnWindowDestroyed(CefRefPtr<CefWindow> window) override {
    auto browser = browser_view_->GetBrowser();
    browser_view_ = nullptr;
    handler_->OnWindowDestroyed(browser_id_);
  }

  bool CanClose(CefRefPtr<CefWindow> window) override {
    // Allow the window to close if the browser says it's OK.
    CefRefPtr<CefBrowser> browser = browser_view_->GetBrowser();
    if (browser) {
      return browser->GetHost()->TryCloseBrowser();
    }
    return true;
  }

 private:
  TestWindowDelegate(TestHandler* handler,
                     CefRefPtr<CefBrowserView> browser_view,
                     const CefString& title)
      : handler_(handler), browser_view_(browser_view), title_(title) {}

  TestHandler* const handler_;
  CefRefPtr<CefBrowserView> browser_view_;
  int browser_id_ = 0;
  CefString title_;

  IMPLEMENT_REFCOUNTING(TestWindowDelegate);
  DISALLOW_COPY_AND_ASSIGN(TestWindowDelegate);
};

// Delegate implementation for the CefBrowserView.
class TestBrowserViewDelegate : public CefBrowserViewDelegate {
 public:
  explicit TestBrowserViewDelegate(TestHandler* handler) : handler_(handler) {}

  // CefBrowserViewDelegate methods:

  void OnBrowserDestroyed(CefRefPtr<CefBrowserView> browser_view,
                          CefRefPtr<CefBrowser> browser) override {
#if VERBOSE_DEBUGGING
    LOG(INFO) << handler_->debug_string_prefix() << browser->GetIdentifier()
              << ": OnBrowserDestroyed";
#endif
    // Always close the containing Window when the browser is destroyed.
    if (auto window = browser_view->GetWindow()) {
#if VERBOSE_DEBUGGING
      LOG(INFO) << handler_->debug_string_prefix() << browser->GetIdentifier()
                << ": OnBrowserDestroyed Close";
#endif
      window->Close();
    }
  }

  CefRefPtr<CefBrowserViewDelegate> GetDelegateForPopupBrowserView(
      CefRefPtr<CefBrowserView> browser_view,
      const CefBrowserSettings& settings,
      CefRefPtr<CefClient> client,
      bool is_devtools) override {
    if (client.get() == handler_) {
      // Use the same Delegate when using the same TestHandler instance.
      return this;
    }

    // Return a new Delegate when using a different TestHandler instance.
    auto* handler = static_cast<TestHandler*>(client.get());
    return new TestBrowserViewDelegate(handler);
  }

  bool OnPopupBrowserViewCreated(CefRefPtr<CefBrowserView> browser_view,
                                 CefRefPtr<CefBrowserView> popup_browser_view,
                                 bool is_devtools) override {
    // The popup may use a different TestHandler instance.
    auto* handler = static_cast<TestHandler*>(
        popup_browser_view->GetBrowser()->GetHost()->GetClient().get());

    // Create our own Window for popups. It will show itself after creation.
    TestWindowDelegate::CreateBrowserWindow(handler, popup_browser_view,
                                            is_devtools ? "DevTools" : "Popup");
    return true;
  }

 private:
  TestHandler* const handler_;

  IMPLEMENT_REFCOUNTING(TestBrowserViewDelegate);
  DISALLOW_COPY_AND_ASSIGN(TestBrowserViewDelegate);
};

}  // namespace

// TestHandler::CompletionState

TestHandler::CompletionState::CompletionState(int total)
    : total_(total), count_(0) {
  event_ = CefWaitableEvent::CreateWaitableEvent(true, false);
}

void TestHandler::CompletionState::TestComplete() {
  if (++count_ == total_) {
    count_ = 0;

    // Signal that the test is now complete. Do not access any object members
    // after this call because |this| might be deleted.
    event_->Signal();
  }
}

void TestHandler::CompletionState::WaitForTests() {
  // Wait for the test to complete
  event_->Wait();

  // Reset the event so the same test can be executed again.
  event_->Reset();
}

// TestHandler::Collection

TestHandler::Collection::Collection(CompletionState* completion_state)
    : completion_state_(completion_state) {
  EXPECT_TRUE(completion_state_);
}

void TestHandler::Collection::AddTestHandler(TestHandler* test_handler) {
  EXPECT_EQ(test_handler->completion_state_, completion_state_);
  handler_list_.push_back(test_handler);
}

void TestHandler::Collection::ExecuteTests() {
  EXPECT_GT(handler_list_.size(), 0UL);

  TestHandlerList::const_iterator it;

  it = handler_list_.begin();
  for (; it != handler_list_.end(); ++it) {
    (*it)->SetupTest();
  }

  completion_state_->WaitForTests();

  it = handler_list_.begin();
  for (; it != handler_list_.end(); ++it) {
    (*it)->RunTest();
  }

  completion_state_->WaitForTests();
}

// TestHandler::UIThreadHelper

TestHandler::UIThreadHelper::UIThreadHelper() : weak_ptr_factory_(this) {}

void TestHandler::UIThreadHelper::PostTask(base::OnceClosure task) {
  EXPECT_UI_THREAD();
  CefPostTask(TID_UI,
              base::BindOnce(&UIThreadHelper::TaskHelper,
                             weak_ptr_factory_.GetWeakPtr(), std::move(task)));
}

void TestHandler::UIThreadHelper::PostDelayedTask(base::OnceClosure task,
                                                  int delay_ms) {
  EXPECT_UI_THREAD();
  CefPostDelayedTask(
      TID_UI,
      base::BindOnce(&UIThreadHelper::TaskHelper,
                     weak_ptr_factory_.GetWeakPtr(), std::move(task)),
      delay_ms);
}

void TestHandler::UIThreadHelper::TaskHelper(base::OnceClosure task) {
  EXPECT_UI_THREAD();
  std::move(task).Run();
}

// TestHandler

int TestHandler::browser_count_ = 0;

TestHandler::TestHandler(CompletionState* completion_state)
    : debug_string_prefix_(MakeDebugStringPrefix()) {
  if (completion_state) {
    completion_state_ = completion_state;
    completion_state_owned_ = false;
  } else {
    completion_state_ = new CompletionState(1);
    completion_state_owned_ = true;
  }
}

TestHandler::~TestHandler() {
  DCHECK(!ui_thread_helper_.get());
  if (destroy_test_expected_) {
    EXPECT_TRUE(destroy_test_called_);
  } else {
    EXPECT_FALSE(destroy_test_called_);
  }
  EXPECT_TRUE(browser_map_.empty());
  EXPECT_EQ(0U, window_count_);
  EXPECT_TRUE(browser_status_map_.empty());

  if (completion_state_owned_) {
    delete completion_state_;
  }

  if (destroy_event_) {
    destroy_event_->Signal();
  }
}

void TestHandler::OnAfterCreated(CefRefPtr<CefBrowser> browser) {
  EXPECT_UI_THREAD();

  const int browser_id = browser->GetIdentifier();
  EXPECT_EQ(browser_map_.find(browser_id), browser_map_.end());
  if (browser_map_.empty()) {
    first_browser_id_ = browser_id;
    first_browser_ = browser;
  }
  browser_map_.insert(std::make_pair(browser_id, browser));

  OnCreated(browser_id, NT_BROWSER);
}

void TestHandler::OnBeforeClose(CefRefPtr<CefBrowser> browser) {
  EXPECT_UI_THREAD();

  // Free the browser pointer so that the browser can be destroyed.
  const int browser_id = browser->GetIdentifier();
  BrowserMap::iterator it = browser_map_.find(browser_id);
  EXPECT_NE(it, browser_map_.end());
  browser_map_.erase(it);

  if (browser_id == first_browser_id_) {
    first_browser_id_ = 0;
    first_browser_ = nullptr;
  }

  OnClosed(browser_id, NT_BROWSER);
}

void TestHandler::OnWindowCreated(int browser_id) {
  CHECK(UseViews());
  EXPECT_UI_THREAD();
  window_count_++;
  OnCreated(browser_id, NT_WINDOW);
}

void TestHandler::OnWindowDestroyed(int browser_id) {
  CHECK(UseViews());
  EXPECT_UI_THREAD();
  window_count_--;
  OnClosed(browser_id, NT_WINDOW);
}

void TestHandler::OnCreated(int browser_id, NotifyType type) {
  bool creation_complete = false;

  auto& browser_status = browser_status_map_[browser_id];
  EXPECT_FALSE(browser_status.got_created[type])
      << "Duplicate call to OnCreated(" << browser_id << ", "
      << (type == NT_BROWSER ? "BROWSER" : "WINDOW") << ")";
  browser_status.got_created[type].yes();

  // When using Views, wait for both Browser and Window notifications.
  if (UseViews()) {
    creation_complete = browser_status.got_created[NT_BROWSER] &&
                        browser_status.got_created[NT_WINDOW];
  } else {
    creation_complete = browser_status.got_created[NT_BROWSER];
  }

#if VERBOSE_DEBUGGING
  LOG(INFO) << debug_string_prefix_ << browser_id << ": OnCreated type="
            << (type == NT_BROWSER ? "BROWSER" : "WINDOW")
            << " creation_complete=" << creation_complete;
#endif

  if (creation_complete) {
    browser_count_++;
  }
}

void TestHandler::OnClosed(int browser_id, NotifyType type) {
  bool close_complete = false;

  auto& browser_status = browser_status_map_[browser_id];
  EXPECT_FALSE(browser_status.got_closed[type])
      << "Duplicate call to OnClosed(" << browser_id << ", "
      << (type == NT_BROWSER ? "BROWSER" : "WINDOW") << ")";
  browser_status.got_closed[type].yes();

  // When using Views, wait for both Browser and Window notifications.
  if (UseViews()) {
    close_complete = browser_status.got_closed[NT_BROWSER] &&
                     browser_status.got_closed[NT_WINDOW];
  } else {
    close_complete = browser_status.got_closed[NT_BROWSER];
  }

  // Test is complete if no Browsers/Windows are remaining.
  const bool test_complete =
      close_complete &&
      (UseViews() ? window_count_ == 0 : browser_map_.empty());

#if VERBOSE_DEBUGGING
  LOG(INFO) << debug_string_prefix_ << browser_id
            << ": OnClosed type=" << (type == NT_BROWSER ? "BROWSER" : "WINDOW")
            << " close_complete=" << close_complete
            << " test_complete=" << test_complete;
#endif

  if (close_complete) {
    browser_status_map_.erase(browser_id);
  }

  if (test_complete && signal_completion_when_all_browsers_close_) {
    // Signal that the test is now complete. May result in |this| being deleted.
    TestComplete();
  }

  if (close_complete) {
    browser_count_--;
  }
}

std::string TestHandler::MakeDebugStringPrefix() const {
#if VERBOSE_DEBUGGING
  std::stringstream ss;
  ss << "TestHandler [0x" << std::hex << this << "]: ";
  return ss.str();
#else
  return std::string();
#endif
}

namespace {

CefResponse::HeaderMap ToCefHeaderMap(
    const ResourceContent::HeaderMap& headerMap) {
  CefResponse::HeaderMap result;
  ResourceContent::HeaderMap::const_iterator it = headerMap.begin();
  for (; it != headerMap.end(); ++it) {
    result.insert(std::pair<CefString, CefString>(it->first, it->second));
  }
  return result;
}

}  // namespace

CefRefPtr<CefResourceHandler> TestHandler::GetResourceHandler(
    CefRefPtr<CefBrowser> browser,
    CefRefPtr<CefFrame> frame,
    CefRefPtr<CefRequest> request) {
  EXPECT_IO_THREAD();

  if (resource_map_.size() > 0) {
    const std::string& url = test_request::GetPathURL(request->GetURL());
    ResourceMap::const_iterator it = resource_map_.find(url);
    if (it != resource_map_.end()) {
      // Return the previously mapped resource
      CefRefPtr<CefStreamReader> stream = CefStreamReader::CreateForData(
          static_cast<void*>(const_cast<char*>(it->second.content().c_str())),
          it->second.content().length());
      return new CefStreamResourceHandler(
          200, "OK", it->second.mimeType(),
          ToCefHeaderMap(it->second.headerMap()), stream);
    }
  }

  return nullptr;
}

void TestHandler::OnRenderProcessTerminated(CefRefPtr<CefBrowser> browser,
                                            TerminationStatus status) {
  LOG(WARNING) << "OnRenderProcessTerminated: status = " << status << ".";
}

CefRefPtr<CefBrowser> TestHandler::GetBrowser() const {
  return first_browser_;
}

int TestHandler::GetBrowserId() const {
  return first_browser_id_;
}

void TestHandler::GetAllBrowsers(BrowserMap* map) {
  EXPECT_UI_THREAD();
  EXPECT_TRUE(map);
  *map = browser_map_;
}

void TestHandler::ExecuteTest() {
  EXPECT_EQ(completion_state_->total(), 1);

  // Reset any state from the previous run.
  if (destroy_test_called_) {
    destroy_test_called_ = false;
  }

  // Run the test.
  RunTest();

  // Wait for the test to complete.
  completion_state_->WaitForTests();
}

void TestHandler::SetupComplete() {
  // Signal that the test setup is complete.
  completion_state_->TestComplete();
}

void TestHandler::DestroyTest() {
  if (!CefCurrentlyOn(TID_UI)) {
    CefPostTask(TID_UI, base::BindOnce(&TestHandler::DestroyTest, this));
    return;
  }

  EXPECT_TRUE(destroy_test_expected_);
  if (destroy_test_called_) {
    return;
  }
  destroy_test_called_ = true;

  if (!browser_map_.empty()) {
    // Use a copy of the map since the original may be modified while we're
    // iterating.
    BrowserMap browser_map = browser_map_;

    // Tell all browsers to close.
    BrowserMap::const_iterator it = browser_map.begin();
    for (; it != browser_map.end(); ++it) {
      CloseBrowser(it->second, false);
    }
  }

  if (ui_thread_helper_.get()) {
    ui_thread_helper_.reset(nullptr);
  }
}

void TestHandler::OnTestTimeout(int timeout_ms, bool treat_as_error) {
  EXPECT_UI_THREAD();
  if (treat_as_error) {
    EXPECT_TRUE(false) << "Test timed out after " << timeout_ms << "ms";
  }
  DestroyTest();
}

void TestHandler::CreateBrowser(const CefString& url,
                                CefRefPtr<CefRequestContext> request_context,
                                CefRefPtr<CefDictionaryValue> extra_info) {
  if (UseViews() && !CefCurrentlyOn(TID_UI)) {
    // Views classes must be accessed on the UI thread.
    CefPostTask(TID_UI, base::BindOnce(&TestHandler::CreateBrowser, this, url,
                                       request_context, extra_info));
    return;
  }

  CefWindowInfo windowInfo;
  CefBrowserSettings settings;

  if (UseViews()) {
    // Create the BrowserView.
    CefRefPtr<CefBrowserView> browser_view = CefBrowserView::CreateBrowserView(
        this, url, settings, extra_info, request_context,
        new TestBrowserViewDelegate(this));

    // Create the Window. It will show itself after creation.
    TestWindowDelegate::CreateBrowserWindow(this, browser_view, std::string());
  } else {
#if defined(OS_WIN)
    windowInfo.SetAsPopup(nullptr, "CefUnitTest");
    windowInfo.style |= WS_VISIBLE;
#endif
    CefBrowserHost::CreateBrowser(windowInfo, this, url, settings, extra_info,
                                  request_context);
  }
}

// static
void TestHandler::CloseBrowser(CefRefPtr<CefBrowser> browser,
                               bool force_close) {
#if VERBOSE_DEBUGGING
  LOG(INFO) << "TestHandler: " << browser->GetIdentifier()
            << ": CloseBrowser force_close=" << force_close;
#endif
  browser->GetHost()->CloseBrowser(force_close);
}

void TestHandler::AddResource(const std::string& url,
                              const std::string& content,
                              const std::string& mime_type,
                              const ResourceContent::HeaderMap& header_map) {
  ResourceContent rc = ResourceContent(content, mime_type, header_map);
  AddResourceEx(url, rc);
}

void TestHandler::AddResourceEx(const std::string& url,
                                const ResourceContent& content) {
  if (!CefCurrentlyOn(TID_IO)) {
    CefPostTask(TID_IO, base::BindOnce(&TestHandler::AddResourceEx, this, url,
                                       content));
    return;
  }

  // Ignore the query component, if any.
  std::string urlStr = url;
  size_t idx = urlStr.find('?');
  if (idx > 0) {
    urlStr = urlStr.substr(0, idx);
  }

  resource_map_.insert(std::make_pair(urlStr, content));
}

void TestHandler::ClearResources() {
  if (!CefCurrentlyOn(TID_IO)) {
    CefPostTask(TID_IO, base::BindOnce(&TestHandler::ClearResources, this));
    return;
  }

  resource_map_.clear();
}

void TestHandler::SetTestTimeout(int timeout_ms, bool treat_as_error) {
  if (!CefCurrentlyOn(TID_UI)) {
    CefPostTask(TID_UI, base::BindOnce(&TestHandler::SetTestTimeout, this,
                                       timeout_ms, treat_as_error));
    return;
  }

  if (destroy_test_called_) {
    // No need to set the timeout if the test has already completed.
    return;
  }

  const auto timeout = GetConfiguredTestTimeout(timeout_ms);
  if (treat_as_error && !timeout) {
    return;
  }

  // Use a weak reference to |this| via UIThreadHelper so that the TestHandler
  // can be destroyed before the timeout expires.
  GetUIThreadHelper()->PostDelayedTask(
      base::BindOnce(&TestHandler::OnTestTimeout, base::Unretained(this),
                     *timeout, treat_as_error),
      *timeout);
}

void TestHandler::TestComplete() {
  if (!CefCurrentlyOn(TID_UI)) {
    CefPostTask(TID_UI, base::BindOnce(&TestHandler::TestComplete, this));
    return;
  }

#if VERBOSE_DEBUGGING
  LOG(INFO) << debug_string_prefix_ << "TestComplete";
#endif

  EXPECT_TRUE(browser_map_.empty());
  completion_state_->TestComplete();
}

TestHandler::UIThreadHelper* TestHandler::GetUIThreadHelper() {
  EXPECT_UI_THREAD();
  CHECK(!destroy_test_called_);

  if (!ui_thread_helper_.get()) {
    ui_thread_helper_.reset(new UIThreadHelper());
  }
  return ui_thread_helper_.get();
}

// global functions

bool TestFailed() {
  CefRefPtr<CefCommandLine> command_line =
      CefCommandLine::GetGlobalCommandLine();
  if (command_line->HasSwitch("single-process")) {
    // Check for a failure on the current test only.
    return ::testing::UnitTest::GetInstance()
        ->current_test_info()
        ->result()
        ->Failed();
  } else {
    // Check for any global failure.
    return ::testing::UnitTest::GetInstance()->Failed();
  }
}
