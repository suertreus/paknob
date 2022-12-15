#include <algorithm>
#include <cassert>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "absl/functional/any_invocable.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "pulse/context.h"
#include "pulse/def.h"
#include "pulse/introspect.h"
#include "pulse/mainloop-api.h"
#include "pulse/mainloop-signal.h"
#include "pulse/mainloop.h"
#include "pulse/operation.h"
#include "pulse/volume.h"

namespace {
using UniqueMainloop =
    std::unique_ptr<pa_mainloop, absl::AnyInvocable<void(pa_mainloop *)>>;
UniqueMainloop NewUniqueMainloop() {
  return UniqueMainloop(pa_mainloop_new(), [](pa_mainloop *const m) {
    if (!m) return;
    pa_signal_done();
    pa_mainloop_free(m);
  });
}
using UniqueContext =
    std::unique_ptr<pa_context, absl::AnyInvocable<void(pa_context *)>>;
UniqueContext NewUniqueContext(pa_mainloop_api *const api,
                               const char *const name) {
  return UniqueContext(pa_context_new(api, name), [](pa_context *const ctx) {
    if (ctx) pa_context_unref(ctx);
  });
}
using UniqueOperation =
    std::unique_ptr<pa_operation, absl::AnyInvocable<void(pa_operation *)>>;
UniqueOperation WrapUniqueOperation(pa_operation *const op) {
  return UniqueOperation(op, [](pa_operation *const op) {
    if (op) pa_operation_unref(op);
  });
}

template <typename T>
class Caster {
 public:
  static T *Cast(void *const userdata) {
    return reinterpret_cast<T *>(userdata);
  }
};

struct SinkTraits {
  using InfoT = pa_sink_info;
  static inline constexpr char kDefaultName[] = "@DEFAULT_SINK@";
  static inline constexpr auto GetInfo = pa_context_get_sink_info_by_name;
  static inline constexpr auto SetVolume = pa_context_set_sink_volume_by_name;
  static inline constexpr auto SetMute = pa_context_set_sink_mute_by_name;
};
struct SourceTraits {
  using InfoT = pa_source_info;
  static inline constexpr char kDefaultName[] = "@DEFAULT_SOURCE@";
  static inline constexpr auto GetInfo = pa_context_get_source_info_by_name;
  static inline constexpr auto SetVolume = pa_context_set_source_volume_by_name;
  static inline constexpr auto SetMute = pa_context_set_source_mute_by_name;
};

class Subcommand {
 public:
  static std::unique_ptr<Subcommand> Build(
      const absl::Span<const absl::string_view> args);
  virtual ~Subcommand() = default;
  static std::string Usage(absl::string_view argv0);
  virtual void Run(pa_context *) = 0;
  void quit(int ret) { api_->quit(api_, ret); }
  [[nodiscard]] pa_mainloop_api *api() const { return api_; }
  void set_api(pa_mainloop_api *api) {
    assert(!api_);
    api_ = api;
  }

 protected:
  Subcommand() : api_{nullptr} {}
  static bool IsValid(absl::string_view name,
                      absl::Span<const absl::string_view> &args) {
    if (args.empty()) return false;
    if (args.front() != name) return false;
    args.remove_prefix(1);
    return true;
  }
  static void Drain(pa_context *const ctx) {
    if (!WrapUniqueOperation(pa_context_drain(ctx, DrainCB, nullptr)))
      pa_context_disconnect(ctx);
  }
  static void PrintVolume(const pa_volume_t vol) {
    absl::PrintF("%d\n", (vol * 100 + PA_VOLUME_NORM / 2) / PA_VOLUME_NORM);
  }
  static void PrintMute(const bool mute) { absl::PrintF("%d\n", mute ? 1 : 0); }

 private:
  static void DrainCB(pa_context *const ctx, void *) {
    pa_context_disconnect(ctx);
  }
  pa_mainloop_api *api_;
};

template <typename T, typename Traits>
class GetVolumeSubcommand : public Subcommand, private Caster<T> {
 public:
  static std::unique_ptr<T> Build(absl::Span<const absl::string_view> args) {
    if (!IsValid(T::kName, args)) return {};
    if (!args.empty()) return {};
    return std::unique_ptr<T>(new T());
  }
  static std::string Usage(absl::string_view argv0) {
    return absl::StrCat(argv0, " ", T::kName);
  }
  void Run(pa_context *const ctx) final {
    WrapUniqueOperation(
        Traits::GetInfo(ctx, Traits::kDefaultName, GetVolumeCB, this));
  }

 protected:
  explicit GetVolumeSubcommand() {}

 private:
  static void GetVolumeCB(pa_context *const ctx,
                          const typename Traits::InfoT *const info,
                          const int is_last, void *const userdata) {
    const auto sc = T::Cast(userdata);
    if (is_last < 0) return sc->quit(1);
    if (is_last) return;
    PrintVolume(pa_cvolume_avg(&info->volume));
    Drain(ctx);
  }
};
class GetSinkVolumeSubcommand final
    : public GetVolumeSubcommand<GetSinkVolumeSubcommand, SinkTraits> {
 public:
  static inline constexpr absl::string_view kName = "get-sink-volume";
  using GetVolumeSubcommand::GetVolumeSubcommand;
};
class GetSourceVolumeSubcommand final
    : public GetVolumeSubcommand<GetSourceVolumeSubcommand, SourceTraits> {
 public:
  static inline constexpr absl::string_view kName = "get-source-volume";
  using GetVolumeSubcommand::GetVolumeSubcommand;
};

template <typename T, typename Traits>
class SetVolumeSubcommand : public Subcommand, private Caster<T> {
 public:
  static std::unique_ptr<T> Build(absl::Span<const absl::string_view> args) {
    if (!IsValid(T::kName, args)) return {};
    if (args.size() != 1) return {};
    pa_volume_t vol;
    if (!absl::SimpleAtoi(args.front(), &vol)) return {};
    vol = vol * PA_VOLUME_NORM / 100;
    if (!PA_VOLUME_IS_VALID(vol)) return {};
    return std::unique_ptr<T>(new T(vol));
  }
  static std::string Usage(absl::string_view argv0) {
    return absl::StrCat(argv0, " ", T::kName, " <percentage>");
  }
  void Run(pa_context *const ctx) final {
    WrapUniqueOperation(
        Traits::GetInfo(ctx, Traits::kDefaultName, GetVolumeCB, this));
  }

 protected:
  explicit SetVolumeSubcommand(const pa_volume_t vol) : vol_{vol} {}

 private:
  static void GetVolumeCB(pa_context *const ctx,
                          const typename Traits::InfoT *const info,
                          const int is_last, void *const userdata) {
    const auto sc = T::Cast(userdata);
    if (is_last < 0) return sc->quit(1);
    if (is_last) return;
    pa_cvolume cv = info->volume;
    pa_cvolume_set(&cv, info->channel_map.channels, sc->vol_);
    WrapUniqueOperation(
        Traits::SetVolume(ctx, Traits::kDefaultName, &cv, SetVolumeCB, sc));
  }
  static void SetVolumeCB(pa_context *const ctx, const int success,
                          void *const userdata) {
    const auto sc = T::Cast(userdata);
    if (!success) return sc->quit(1);
    PrintVolume(sc->vol_);
    Drain(ctx);
  }

  pa_volume_t vol_;
};
class SetSinkVolumeSubcommand final
    : public SetVolumeSubcommand<SetSinkVolumeSubcommand, SinkTraits> {
 public:
  static inline constexpr absl::string_view kName = "set-sink-volume";
  using SetVolumeSubcommand::SetVolumeSubcommand;
};
class SetSourceVolumeSubcommand final
    : public SetVolumeSubcommand<SetSourceVolumeSubcommand, SourceTraits> {
 public:
  static inline constexpr absl::string_view kName = "set-source-volume";
  using SetVolumeSubcommand::SetVolumeSubcommand;
};

template <typename T, typename Traits, bool dec = false>
class AdjustVolumeSubcommand : public Subcommand, private Caster<T> {
 public:
  static std::unique_ptr<T> Build(absl::Span<const absl::string_view> args) {
    if (!IsValid(T::kName, args)) return {};
    if (args.size() != 1) return {};
    auto arg = args.front();
    bool neg = !arg.empty() && arg.front() == '-';
    if (neg) arg.remove_prefix(1);
    neg = neg != dec;
    pa_volume_t vol;
    if (!absl::SimpleAtoi(arg, &vol)) return {};
    vol = vol * PA_VOLUME_NORM / 100;
    if (!PA_VOLUME_IS_VALID(vol)) return {};
    return std::unique_ptr<T>(new T(neg, vol));
  }
  static std::string Usage(absl::string_view argv0) {
    return absl::StrCat(argv0, " ", T::kName, " <percentage>");
  }
  void Run(pa_context *const ctx) final {
    WrapUniqueOperation(
        Traits::GetInfo(ctx, Traits::kDefaultName, GetVolumeCB, this));
  }

 protected:
  explicit AdjustVolumeSubcommand(const bool neg, const pa_volume_t vol_adj)
      : neg_{neg}, vol_adj_{vol_adj} {}

 private:
  static void GetVolumeCB(pa_context *const ctx,
                          const typename Traits::InfoT *const info,
                          const int is_last, void *const userdata) {
    const auto sc = T::Cast(userdata);
    if (is_last < 0) return sc->quit(1);
    if (is_last) return;
    pa_cvolume cv = info->volume;
    for (int i = 0; i < cv.channels; i++) {
      if (sc->neg_)
        cv.values[i] -= std::min(cv.values[i], sc->vol_adj_);
      else
        cv.values[i] = std::min(cv.values[i] + sc->vol_adj_, PA_VOLUME_MAX);
    }
    sc->vol_ = pa_cvolume_avg(&cv);
    WrapUniqueOperation(
        Traits::SetVolume(ctx, Traits::kDefaultName, &cv, SetVolumeCB, sc));
  }
  static void SetVolumeCB(pa_context *const ctx, const int success,
                          void *const userdata) {
    const auto sc = T::Cast(userdata);
    if (!success) return sc->quit(1);
    PrintVolume(sc->vol_);
    Drain(ctx);
  }

  bool neg_;
  pa_volume_t vol_adj_;
  pa_volume_t vol_;
};
class IncrementSinkVolumeSubcommand final
    : public AdjustVolumeSubcommand<IncrementSinkVolumeSubcommand, SinkTraits> {
 public:
  static inline constexpr absl::string_view kName = "increment-sink-volume";
  using AdjustVolumeSubcommand::AdjustVolumeSubcommand;
};
class DecrementSinkVolumeSubcommand final
    : public AdjustVolumeSubcommand<DecrementSinkVolumeSubcommand, SinkTraits,
                                    true> {
 public:
  static inline constexpr absl::string_view kName = "decrement-sink-volume";
  using AdjustVolumeSubcommand::AdjustVolumeSubcommand;
};
class IncrementSourceVolumeSubcommand final
    : public AdjustVolumeSubcommand<IncrementSourceVolumeSubcommand,
                                    SourceTraits> {
 public:
  static inline constexpr absl::string_view kName = "increment-source-volume";
  using AdjustVolumeSubcommand::AdjustVolumeSubcommand;
};
class DecrementSourceVolumeSubcommand final
    : public AdjustVolumeSubcommand<DecrementSourceVolumeSubcommand,
                                    SourceTraits, true> {
 public:
  static inline constexpr absl::string_view kName = "decrement-source-volume";
  using AdjustVolumeSubcommand::AdjustVolumeSubcommand;
};

template <typename T, typename Traits>
class GetMuteSubcommand : public Subcommand, private Caster<T> {
 public:
  static std::unique_ptr<T> Build(absl::Span<const absl::string_view> args) {
    if (!IsValid(T::kName, args)) return {};
    if (!args.empty()) return {};
    return std::unique_ptr<T>(new T());
  }
  static std::string Usage(absl::string_view argv0) {
    return absl::StrCat(argv0, " ", T::kName);
  }
  void Run(pa_context *const ctx) final {
    WrapUniqueOperation(
        Traits::GetInfo(ctx, Traits::kDefaultName, GetMuteCB, this));
  }

 protected:
  explicit GetMuteSubcommand() {}

 private:
  static void GetMuteCB(pa_context *const ctx,
                        const typename Traits::InfoT *const info,
                        const int is_last, void *const userdata) {
    const auto sc = T::Cast(userdata);
    if (is_last < 0) return sc->quit(1);
    if (is_last) return;
    PrintMute(info->mute);
    Drain(ctx);
  }
};
class GetSinkMuteSubcommand final
    : public GetMuteSubcommand<GetSinkMuteSubcommand, SinkTraits> {
 public:
  static inline constexpr absl::string_view kName = "get-sink-mute";
  using GetMuteSubcommand::GetMuteSubcommand;
};
class GetSourceMuteSubcommand final
    : public GetMuteSubcommand<GetSourceMuteSubcommand, SourceTraits> {
 public:
  static inline constexpr absl::string_view kName = "get-source-mute";
  using GetMuteSubcommand::GetMuteSubcommand;
};

template <typename T, typename Traits>
class SetMuteSubcommand : public Subcommand, private Caster<T> {
 public:
  static std::unique_ptr<T> Build(absl::Span<const absl::string_view> args) {
    if (!IsValid(T::kName, args)) return {};
    if (args.size() != 1) return {};
    bool mute;
    if (!absl::SimpleAtob(args.front(), &mute)) return {};
    return std::unique_ptr<T>(new T(mute));
  }
  static std::string Usage(absl::string_view argv0) {
    return absl::StrCat(argv0, " ", T::kName, " <0|1>");
  }
  void Run(pa_context *const ctx) final {
    WrapUniqueOperation(
        Traits::GetInfo(ctx, Traits::kDefaultName, GetInfoCB, this));
  }

 protected:
  explicit SetMuteSubcommand(const bool mute) : mute_{mute} {}

 private:
  static void GetInfoCB(pa_context *const ctx,
                        const typename Traits::InfoT *const info,
                        const int is_last, void *const userdata) {
    const auto sc = T::Cast(userdata);
    if (is_last < 0) return sc->quit(1);
    if (is_last) return;
    sc->vol_ = sc->mute_ ? PA_VOLUME_MUTED : pa_cvolume_avg(&info->volume);
    WrapUniqueOperation(
        Traits::SetMute(ctx, Traits::kDefaultName, sc->mute_, SetMuteCB, sc));
  }
  static void SetMuteCB(pa_context *const ctx, const int success,
                        void *const userdata) {
    const auto sc = T::Cast(userdata);
    if (!success) return sc->quit(1);
    PrintVolume(sc->vol_);
    Drain(ctx);
  }

  bool mute_;
  pa_volume_t vol_;
};
class SetSinkMuteSubcommand final
    : public SetMuteSubcommand<SetSinkMuteSubcommand, SinkTraits> {
 public:
  static inline constexpr absl::string_view kName = "set-sink-mute";
  using SetMuteSubcommand::SetMuteSubcommand;
};
class SetSourceMuteSubcommand final
    : public SetMuteSubcommand<SetSourceMuteSubcommand, SourceTraits> {
 public:
  static inline constexpr absl::string_view kName = "set-source-mute";
  using SetMuteSubcommand::SetMuteSubcommand;
};

template <typename T, typename Traits>
class ToggleMuteSubcommand : public Subcommand, private Caster<T> {
 public:
  static std::unique_ptr<T> Build(absl::Span<const absl::string_view> args) {
    if (!IsValid(T::kName, args)) return {};
    if (!args.empty()) return {};
    return std::unique_ptr<T>(new T());
  }
  static std::string Usage(absl::string_view argv0) {
    return absl::StrCat(argv0, " ", T::kName);
  }
  void Run(pa_context *const ctx) final {
    WrapUniqueOperation(
        Traits::GetInfo(ctx, Traits::kDefaultName, GetInfoCB, this));
  }

 protected:
  explicit ToggleMuteSubcommand() {}

 private:
  static void GetInfoCB(pa_context *const ctx,
                        const typename Traits::InfoT *const info,
                        const int is_last, void *const userdata) {
    const auto sc = T::Cast(userdata);
    if (is_last < 0) return sc->quit(1);
    if (is_last) return;
    sc->vol_ = !info->mute ? PA_VOLUME_MUTED : pa_cvolume_avg(&info->volume);
    WrapUniqueOperation(
        Traits::SetMute(ctx, Traits::kDefaultName, !info->mute, SetMuteCB, sc));
  }
  static void SetMuteCB(pa_context *const ctx, const int success,
                        void *const userdata) {
    const auto sc = T::Cast(userdata);
    if (!success) return sc->quit(1);
    PrintVolume(sc->vol_);
    Drain(ctx);
  }

  pa_volume_t vol_;
};
class ToggleSinkMuteSubcommand final
    : public ToggleMuteSubcommand<ToggleSinkMuteSubcommand, SinkTraits> {
 public:
  static inline constexpr absl::string_view kName = "toggle-sink-mute";
  using ToggleMuteSubcommand::ToggleMuteSubcommand;
};
class ToggleSourceMuteSubcommand final
    : public ToggleMuteSubcommand<ToggleSourceMuteSubcommand, SourceTraits> {
 public:
  static inline constexpr absl::string_view kName = "toggle-source-mute";
  using ToggleMuteSubcommand::ToggleMuteSubcommand;
};

std::unique_ptr<Subcommand> Subcommand::Build(
    const absl::Span<const absl::string_view> args) {
  if (auto cmd = GetSinkVolumeSubcommand::Build(args); cmd) return cmd;
  if (auto cmd = SetSinkVolumeSubcommand::Build(args); cmd) return cmd;
  if (auto cmd = IncrementSinkVolumeSubcommand::Build(args); cmd) return cmd;
  if (auto cmd = DecrementSinkVolumeSubcommand::Build(args); cmd) return cmd;
  if (auto cmd = GetSourceVolumeSubcommand::Build(args); cmd) return cmd;
  if (auto cmd = SetSourceVolumeSubcommand::Build(args); cmd) return cmd;
  if (auto cmd = IncrementSourceVolumeSubcommand::Build(args); cmd) return cmd;
  if (auto cmd = DecrementSourceVolumeSubcommand::Build(args); cmd) return cmd;
  if (auto cmd = GetSinkMuteSubcommand::Build(args); cmd) return cmd;
  if (auto cmd = SetSinkMuteSubcommand::Build(args); cmd) return cmd;
  if (auto cmd = ToggleSinkMuteSubcommand::Build(args); cmd) return cmd;
  if (auto cmd = GetSourceMuteSubcommand::Build(args); cmd) return cmd;
  if (auto cmd = SetSourceMuteSubcommand::Build(args); cmd) return cmd;
  if (auto cmd = ToggleSourceMuteSubcommand::Build(args); cmd) return cmd;
  return {};
}

std::string Subcommand::Usage(absl::string_view argv0) {
  return absl::StrCat(
      "Usage:\n"
      "  ",
      GetSinkVolumeSubcommand::Usage(argv0),
      "\n"
      "  ",
      SetSinkVolumeSubcommand::Usage(argv0),
      "\n"
      "  ",
      IncrementSinkVolumeSubcommand::Usage(argv0),
      "\n"
      "  ",
      DecrementSinkVolumeSubcommand::Usage(argv0),
      "\n"
      "  ",
      GetSourceVolumeSubcommand::Usage(argv0),
      "\n"
      "  ",
      SetSourceVolumeSubcommand::Usage(argv0),
      "\n"
      "  ",
      IncrementSourceVolumeSubcommand::Usage(argv0),
      "\n"
      "  ",
      DecrementSourceVolumeSubcommand::Usage(argv0),
      "\n"
      "  ",
      GetSinkMuteSubcommand::Usage(argv0),
      "\n"
      "  ",
      SetSinkMuteSubcommand::Usage(argv0),
      "\n"
      "  ",
      ToggleSinkMuteSubcommand::Usage(argv0),
      "\n"
      "  ",
      GetSourceMuteSubcommand::Usage(argv0),
      "\n"
      "  ",
      SetSourceMuteSubcommand::Usage(argv0),
      "\n"
      "  ",
      ToggleSourceMuteSubcommand::Usage(argv0), "\n");
}

void ContextCB(pa_context *const ctx, void *const userdata) {
  Subcommand *const sc = reinterpret_cast<Subcommand *>(userdata);
  switch (pa_context_get_state(ctx)) {
    case PA_CONTEXT_CONNECTING:
    case PA_CONTEXT_AUTHORIZING:
    case PA_CONTEXT_SETTING_NAME:
      return;
    case PA_CONTEXT_READY:
      return sc->Run(ctx);
    case PA_CONTEXT_TERMINATED:
      return sc->quit(0);
    case PA_CONTEXT_FAILED:
    default:
      return sc->quit(1);
  }
}

void HandleSignal(pa_mainloop_api *const m, pa_signal_event *, int, void *) {
  if (m) m->quit(m, 0);
  exit(0);
}

std::vector<absl::string_view> Args(const int argc, char **const argv) {
  if (!argv) return {};
  std::vector<absl::string_view> args;
  args.reserve(argc);
  for (int i = 0; i < argc && argv[i]; i++) args.emplace_back(argv[i]);
  return args;
}
}  // namespace

int main(const int argc, char **const argv) {
  auto args = Args(argc, argv);
  auto sc = Subcommand::Build(absl::MakeSpan(args).subspan(1));
  if (!sc) {
    fputs(Subcommand::Usage(args.empty() ? "paknob" : args.front()).c_str(),
          stderr);
    return EXIT_FAILURE;
  }
  const auto m = NewUniqueMainloop();
  if (!m) return EXIT_FAILURE;
  sc->set_api(pa_mainloop_get_api(m.get()));
  if (!sc->api()) return EXIT_FAILURE;
  if (pa_signal_init(sc->api()) != 0) return EXIT_FAILURE;
  pa_signal_new(SIGINT, HandleSignal, nullptr);
  pa_signal_new(SIGTERM, HandleSignal, nullptr);
#ifdef SIGPIPE
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  if (sigaction(SIGPIPE, nullptr, &sa) != 0) return EXIT_FAILURE;
  sa.sa_handler = SIG_IGN;
  if (sigaction(SIGPIPE, &sa, nullptr) != 0) return EXIT_FAILURE;
#endif
  const auto ctx = NewUniqueContext(sc->api(), /* name = */ nullptr);
  if (!ctx) return EXIT_FAILURE;
  pa_context_set_state_callback(ctx.get(), ContextCB, sc.get());
  if (pa_context_connect(ctx.get(), /* server = */ nullptr,
                         static_cast<pa_context_flags_t>(0),
                         /* api = */ nullptr) < 0)
    return EXIT_FAILURE;
  int ret = EXIT_SUCCESS;
  if (pa_mainloop_run(m.get(), &ret) < 0) return EXIT_FAILURE;
  return ret;
}
