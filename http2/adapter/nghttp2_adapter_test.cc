#include "http2/adapter/nghttp2_adapter.h"

#include "http2/adapter/mock_http2_visitor.h"
#include "http2/adapter/test_frame_sequence.h"
#include "http2/adapter/test_utils.h"
#include "common/platform/api/quiche_test.h"

namespace http2 {
namespace adapter {
namespace test {
namespace {

using testing::_;

enum FrameType {
  DATA,
  HEADERS,
  PRIORITY,
  RST_STREAM,
  SETTINGS,
  PUSH_PROMISE,
  PING,
  GOAWAY,
  WINDOW_UPDATE,
};

TEST(NgHttp2AdapterTest, ClientConstruction) {
  testing::StrictMock<MockHttp2Visitor> visitor;
  auto adapter = NgHttp2Adapter::CreateClientAdapter(visitor);
  ASSERT_NE(nullptr, adapter);
  EXPECT_TRUE(adapter->session().want_read());
  EXPECT_FALSE(adapter->session().want_write());
}

TEST(NgHttp2AdapterTest, ClientHandlesFrames) {
  DataSavingVisitor visitor;
  auto adapter = NgHttp2Adapter::CreateClientAdapter(visitor);
  adapter->Send();
  EXPECT_THAT(visitor.data(),
              testing::StrEq(spdy::kHttp2ConnectionHeaderPrefix));
  visitor.Clear();

  const std::string initial_frames = TestFrameSequence()
                                         .ServerPreface()
                                         .Ping(42)
                                         .WindowUpdate(0, 1000)
                                         .Serialize();
  testing::InSequence s;

  // Server preface (empty SETTINGS)
  EXPECT_CALL(visitor, OnFrameHeader(0, 0, SETTINGS, 0));
  EXPECT_CALL(visitor, OnSettingsStart());
  EXPECT_CALL(visitor, OnSettingsEnd());

  EXPECT_CALL(visitor, OnFrameHeader(0, 8, PING, 0));
  EXPECT_CALL(visitor, OnPing(42, false));
  EXPECT_CALL(visitor, OnFrameHeader(0, 4, WINDOW_UPDATE, 0));
  EXPECT_CALL(visitor, OnWindowUpdate(0, 1000));

  const ssize_t initial_result = adapter->ProcessBytes(initial_frames);
  EXPECT_EQ(initial_frames.size(), initial_result);

  EXPECT_EQ(adapter->GetPeerConnectionWindow(),
            kDefaultInitialStreamWindowSize + 1000);
  // Some bytes should have been serialized.
  adapter->Send();
  EXPECT_THAT(visitor.data(), EqualsFrames({spdy::SpdyFrameType::SETTINGS,
                                            spdy::SpdyFrameType::PING}));
  visitor.Clear();

  const std::vector<const Header> headers1 =
      ToHeaders({{":method", "GET"},
                 {":scheme", "http"},
                 {":authority", "example.com"},
                 {":path", "/this/is/request/one"}});
  const auto nvs1 = GetNghttp2Nvs(headers1);

  const std::vector<const Header> headers2 =
      ToHeaders({{":method", "GET"},
                 {":scheme", "http"},
                 {":authority", "example.com"},
                 {":path", "/this/is/request/two"}});
  const auto nvs2 = GetNghttp2Nvs(headers2);

  const std::vector<const Header> headers3 =
      ToHeaders({{":method", "GET"},
                 {":scheme", "http"},
                 {":authority", "example.com"},
                 {":path", "/this/is/request/three"}});
  const auto nvs3 = GetNghttp2Nvs(headers3);

  const int32_t stream_id1 =
      nghttp2_submit_request(adapter->session().raw_ptr(), nullptr, nvs1.data(),
                             nvs1.size(), nullptr, nullptr);
  ASSERT_GT(stream_id1, 0);
  QUICHE_LOG(INFO) << "Created stream: " << stream_id1;

  const int32_t stream_id2 =
      nghttp2_submit_request(adapter->session().raw_ptr(), nullptr, nvs2.data(),
                             nvs2.size(), nullptr, nullptr);
  ASSERT_GT(stream_id2, 0);
  QUICHE_LOG(INFO) << "Created stream: " << stream_id2;

  const int32_t stream_id3 =
      nghttp2_submit_request(adapter->session().raw_ptr(), nullptr, nvs3.data(),
                             nvs3.size(), nullptr, nullptr);
  ASSERT_GT(stream_id3, 0);
  QUICHE_LOG(INFO) << "Created stream: " << stream_id3;

  adapter->Send();
  EXPECT_THAT(visitor.data(), EqualsFrames({spdy::SpdyFrameType::HEADERS,
                                            spdy::SpdyFrameType::HEADERS,
                                            spdy::SpdyFrameType::HEADERS}));
  visitor.Clear();

  const std::string stream_frames =
      TestFrameSequence()
          .Headers(1,
                   {{":status", "200"},
                    {"server", "my-fake-server"},
                    {"date", "Tue, 6 Apr 2021 12:54:01 GMT"}},
                   /*fin=*/false)
          .Data(1, "This is the response body.")
          .RstStream(3, Http2ErrorCode::INTERNAL_ERROR)
          .GoAway(5, Http2ErrorCode::ENHANCE_YOUR_CALM, "calm down!!")
          .Serialize();

  EXPECT_CALL(visitor, OnFrameHeader(1, _, HEADERS, 4));
  EXPECT_CALL(visitor, OnBeginHeadersForStream(1));
  EXPECT_CALL(visitor, OnHeaderForStream(1, ":status", "200"));
  EXPECT_CALL(visitor, OnHeaderForStream(1, "server", "my-fake-server"));
  EXPECT_CALL(visitor,
              OnHeaderForStream(1, "date", "Tue, 6 Apr 2021 12:54:01 GMT"));
  EXPECT_CALL(visitor, OnEndHeadersForStream(1));
  EXPECT_CALL(visitor, OnFrameHeader(1, 26, DATA, 0));
  EXPECT_CALL(visitor, OnBeginDataForStream(1, 26));
  EXPECT_CALL(visitor, OnDataForStream(1, "This is the response body."));
  EXPECT_CALL(visitor, OnFrameHeader(3, 4, RST_STREAM, 0));
  EXPECT_CALL(visitor, OnRstStream(3, Http2ErrorCode::INTERNAL_ERROR));
  EXPECT_CALL(visitor, OnCloseStream(3, Http2ErrorCode::INTERNAL_ERROR));
  EXPECT_CALL(visitor, OnFrameHeader(0, 19, GOAWAY, 0));
  EXPECT_CALL(visitor,
              OnGoAway(5, Http2ErrorCode::ENHANCE_YOUR_CALM, "calm down!!"));
  const ssize_t stream_result = adapter->ProcessBytes(stream_frames);
  EXPECT_EQ(stream_frames.size(), stream_result);

  // Even though the client recieved a GOAWAY, streams 1 and 5 are still active.
  EXPECT_TRUE(adapter->session().want_read());

  EXPECT_CALL(visitor, OnFrameHeader(1, 0, DATA, 1));
  EXPECT_CALL(visitor, OnBeginDataForStream(1, 0));
  EXPECT_CALL(visitor, OnEndStream(1));
  EXPECT_CALL(visitor, OnCloseStream(1, Http2ErrorCode::NO_ERROR));
  EXPECT_CALL(visitor, OnFrameHeader(5, 4, RST_STREAM, 0));
  EXPECT_CALL(visitor, OnRstStream(5, Http2ErrorCode::REFUSED_STREAM));
  EXPECT_CALL(visitor, OnCloseStream(5, Http2ErrorCode::REFUSED_STREAM));
  adapter->ProcessBytes(TestFrameSequence()
                            .Data(1, "", true)
                            .RstStream(5, Http2ErrorCode::REFUSED_STREAM)
                            .Serialize());
  // After receiving END_STREAM for 1 and RST_STREAM for 5, the session no
  // longer expects reads.
  EXPECT_FALSE(adapter->session().want_read());

  // Client will not have anything else to write.
  EXPECT_FALSE(adapter->session().want_write());
  adapter->Send();
  EXPECT_THAT(visitor.data(), testing::IsEmpty());
}

TEST(NgHttp2AdapterTest, ClientSubmitRequest) {
  DataSavingVisitor visitor;
  auto adapter = NgHttp2Adapter::CreateClientAdapter(visitor);
  adapter->Send();
  // Client preface does not appear to include the mandatory SETTINGS frame.
  EXPECT_THAT(visitor.data(),
              testing::StrEq(spdy::kHttp2ConnectionHeaderPrefix));
  visitor.Clear();

  const std::string initial_frames =
      TestFrameSequence().ServerPreface().Serialize();
  testing::InSequence s;

  // Server preface (empty SETTINGS)
  EXPECT_CALL(visitor, OnFrameHeader(0, 0, SETTINGS, 0));
  EXPECT_CALL(visitor, OnSettingsStart());
  EXPECT_CALL(visitor, OnSettingsEnd());

  const ssize_t initial_result = adapter->ProcessBytes(initial_frames);
  EXPECT_EQ(initial_frames.size(), initial_result);

  EXPECT_TRUE(adapter->session().want_write());
  adapter->Send();
  EXPECT_THAT(visitor.data(), EqualsFrames({spdy::SpdyFrameType::SETTINGS}));
  visitor.Clear();

  EXPECT_FALSE(adapter->session().want_write());
  const char* kSentinel = "";
  const absl::string_view kBody = "This is an example request body.";
  TestDataFrameSource body1(visitor, kBody, true);
  int stream_id =
      adapter->SubmitRequest(ToHeaders({{":method", "POST"},
                                        {":scheme", "http"},
                                        {":authority", "example.com"},
                                        {":path", "/this/is/request/one"}}),
                             &body1, const_cast<char*>(kSentinel));
  EXPECT_GT(stream_id, 0);
  EXPECT_TRUE(adapter->session().want_write());
  adapter->Send();
  EXPECT_THAT(visitor.data(), EqualsFrames({spdy::SpdyFrameType::HEADERS,
                                            spdy::SpdyFrameType::DATA}));
  EXPECT_THAT(visitor.data(), testing::HasSubstr(kBody));
  EXPECT_FALSE(adapter->session().want_write());
}

TEST(NgHttp2AdapterTest, ServerConstruction) {
  testing::StrictMock<MockHttp2Visitor> visitor;
  auto adapter = NgHttp2Adapter::CreateServerAdapter(visitor);
  ASSERT_NE(nullptr, adapter);
  EXPECT_TRUE(adapter->session().want_read());
  EXPECT_FALSE(adapter->session().want_write());
}

TEST(NgHttp2AdapterTest, ServerHandlesFrames) {
  DataSavingVisitor visitor;
  auto adapter = NgHttp2Adapter::CreateServerAdapter(visitor);

  const std::string frames = TestFrameSequence()
                                 .ClientPreface()
                                 .Ping(42)
                                 .WindowUpdate(0, 1000)
                                 .Headers(1,
                                          {{":method", "POST"},
                                           {":scheme", "https"},
                                           {":authority", "example.com"},
                                           {":path", "/this/is/request/one"}},
                                          /*fin=*/false)
                                 .WindowUpdate(1, 2000)
                                 .Data(1, "This is the request body.")
                                 .Headers(3,
                                          {{":method", "GET"},
                                           {":scheme", "http"},
                                           {":authority", "example.com"},
                                           {":path", "/this/is/request/two"}},
                                          /*fin=*/true)
                                 .RstStream(3, Http2ErrorCode::CANCEL)
                                 .Ping(47)
                                 .Serialize();
  testing::InSequence s;

  // Client preface (empty SETTINGS)
  EXPECT_CALL(visitor, OnFrameHeader(0, 0, SETTINGS, 0));
  EXPECT_CALL(visitor, OnSettingsStart());
  EXPECT_CALL(visitor, OnSettingsEnd());

  EXPECT_CALL(visitor, OnFrameHeader(0, 8, PING, 0));
  EXPECT_CALL(visitor, OnPing(42, false));
  EXPECT_CALL(visitor, OnFrameHeader(0, 4, WINDOW_UPDATE, 0));
  EXPECT_CALL(visitor, OnWindowUpdate(0, 1000));
  EXPECT_CALL(visitor, OnFrameHeader(1, _, HEADERS, 4));
  EXPECT_CALL(visitor, OnBeginHeadersForStream(1));
  EXPECT_CALL(visitor, OnHeaderForStream(1, ":method", "POST"));
  EXPECT_CALL(visitor, OnHeaderForStream(1, ":scheme", "https"));
  EXPECT_CALL(visitor, OnHeaderForStream(1, ":authority", "example.com"));
  EXPECT_CALL(visitor, OnHeaderForStream(1, ":path", "/this/is/request/one"));
  EXPECT_CALL(visitor, OnEndHeadersForStream(1));
  EXPECT_CALL(visitor, OnFrameHeader(1, 4, WINDOW_UPDATE, 0));
  EXPECT_CALL(visitor, OnWindowUpdate(1, 2000));
  EXPECT_CALL(visitor, OnFrameHeader(1, 25, DATA, 0));
  EXPECT_CALL(visitor, OnBeginDataForStream(1, 25));
  EXPECT_CALL(visitor, OnDataForStream(1, "This is the request body."));
  EXPECT_CALL(visitor, OnFrameHeader(3, _, HEADERS, 5));
  EXPECT_CALL(visitor, OnBeginHeadersForStream(3));
  EXPECT_CALL(visitor, OnHeaderForStream(3, ":method", "GET"));
  EXPECT_CALL(visitor, OnHeaderForStream(3, ":scheme", "http"));
  EXPECT_CALL(visitor, OnHeaderForStream(3, ":authority", "example.com"));
  EXPECT_CALL(visitor, OnHeaderForStream(3, ":path", "/this/is/request/two"));
  EXPECT_CALL(visitor, OnEndHeadersForStream(3));
  EXPECT_CALL(visitor, OnEndStream(3));
  EXPECT_CALL(visitor, OnFrameHeader(3, 4, RST_STREAM, 0));
  EXPECT_CALL(visitor, OnRstStream(3, Http2ErrorCode::CANCEL));
  EXPECT_CALL(visitor, OnCloseStream(3, Http2ErrorCode::CANCEL));
  EXPECT_CALL(visitor, OnFrameHeader(0, 8, PING, 0));
  EXPECT_CALL(visitor, OnPing(47, false));

  const ssize_t result = adapter->ProcessBytes(frames);
  EXPECT_EQ(frames.size(), result);

  EXPECT_EQ(adapter->GetPeerConnectionWindow(),
            kDefaultInitialStreamWindowSize + 1000);

  EXPECT_TRUE(adapter->session().want_write());
  // Some bytes should have been serialized.
  adapter->Send();
  // SETTINGS ack, two PING acks.
  EXPECT_THAT(visitor.data(), EqualsFrames({spdy::SpdyFrameType::SETTINGS,
                                            spdy::SpdyFrameType::PING,
                                            spdy::SpdyFrameType::PING}));
}

TEST(NgHttp2AdapterTest, ServerSubmitResponse) {
  DataSavingVisitor visitor;
  auto adapter = NgHttp2Adapter::CreateServerAdapter(visitor);
  EXPECT_FALSE(adapter->session().want_write());

  const std::string frames = TestFrameSequence()
                                 .ClientPreface()
                                 .Headers(1,
                                          {{":method", "GET"},
                                           {":scheme", "https"},
                                           {":authority", "example.com"},
                                           {":path", "/this/is/request/one"}},
                                          /*fin=*/true)
                                 .Serialize();
  testing::InSequence s;

  // Client preface (empty SETTINGS)
  EXPECT_CALL(visitor, OnFrameHeader(0, 0, SETTINGS, 0));
  EXPECT_CALL(visitor, OnSettingsStart());
  EXPECT_CALL(visitor, OnSettingsEnd());
  // Stream 1
  EXPECT_CALL(visitor, OnFrameHeader(1, _, HEADERS, 5));
  EXPECT_CALL(visitor, OnBeginHeadersForStream(1));
  EXPECT_CALL(visitor, OnHeaderForStream(1, ":method", "GET"));
  EXPECT_CALL(visitor, OnHeaderForStream(1, ":scheme", "https"));
  EXPECT_CALL(visitor, OnHeaderForStream(1, ":authority", "example.com"));
  EXPECT_CALL(visitor, OnHeaderForStream(1, ":path", "/this/is/request/one"));
  EXPECT_CALL(visitor, OnEndHeadersForStream(1));
  EXPECT_CALL(visitor, OnEndStream(1));

  const ssize_t result = adapter->ProcessBytes(frames);
  EXPECT_EQ(frames.size(), result);

  // Server will want to send a SETTINGS ack.
  EXPECT_TRUE(adapter->session().want_write());
  adapter->Send();
  EXPECT_THAT(visitor.data(), EqualsFrames({spdy::SpdyFrameType::SETTINGS}));
  visitor.Clear();

  EXPECT_FALSE(adapter->session().want_write());
  const absl::string_view kBody = "This is an example response body.";
  TestDataFrameSource body1(visitor, kBody, true);
  int submit_result = adapter->SubmitResponse(
      1,
      ToHeaders({{":status", "404"},
                 {"x-comment", "I have no idea what you're talking about."}}),
      &body1);
  EXPECT_EQ(submit_result, 0);
  EXPECT_TRUE(adapter->session().want_write());
  EXPECT_CALL(visitor, OnCloseStream(1, Http2ErrorCode::NO_ERROR));
  adapter->Send();
  EXPECT_THAT(visitor.data(), EqualsFrames({spdy::SpdyFrameType::HEADERS,
                                            spdy::SpdyFrameType::DATA}));
  EXPECT_THAT(visitor.data(), testing::HasSubstr(kBody));
  EXPECT_FALSE(adapter->session().want_write());
}

}  // namespace
}  // namespace test
}  // namespace adapter
}  // namespace http2
