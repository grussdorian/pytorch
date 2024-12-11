#pragma once

#include <c10/core/Stream.h>
#include <c10/core/impl/GPUTrace.h>
#include <c10/xpu/XPUFunctions.h>

namespace c10::xpu {

/*
 * Note [Stream Management]
 *
 * An XPUStream is an abstraction of an actual SYCL queue in which SYCL kernel
 * can execute. Currently, there are several pools per device to manage SYCL
 * queue, and a device's pool is lazily created.
 *
 * There are two pools per device. The first pool contains "normal priority"
 * queues. The second pool is the "high priority" queues. There are 32 queues in
 * per pool per device, and when a queue is requested one of these queues is
 * returned round-robin. That is, the first queue requested is at index 0, the
 * second at index 1... to index 31, then index 0 again.
 *
 * This means that if 33 queues are requested, the first and last queues
 * requested are actually the same queue (under the covers) and kernels enqueued
 * on them cannot run concurrently.
 *
 * It is safe to enqueue a kernel on the same queue from two different
 * threads as the SYCL specification described.
 */

static constexpr int max_compile_time_stream_priorities = 2;

/*
 * This serves as a wrapper around c10::Stream and acts as a representation for
 * a SYCL queue, which allows asynchronous execution of XPU tasks.
 */
class C10_XPU_API XPUStream {
 public:
  enum Unchecked { UNCHECKED };

  /// Construct a XPUStream from a Stream. This construction is checked, and
  /// will raise an error if the Stream is not, in fact, a XPU stream.
  explicit XPUStream(Stream stream) : stream_(stream) {
    TORCH_CHECK(stream_.device_type() == DeviceType::XPU);
  }

  /// Construct a XPUStream from a Stream with no error checking.
  explicit XPUStream(Unchecked, Stream stream) : stream_(stream) {}

  bool operator==(const XPUStream& other) const noexcept {
    return unwrap() == other.unwrap();
  }

  bool operator!=(const XPUStream& other) const noexcept {
    return unwrap() != other.unwrap();
  }

  /// Implicit conversion to sycl::queue&.
  operator sycl::queue&() const {
    return queue();
  }

  /// Implicit conversion to Stream (a.k.a., forget that the stream is a
  /// XPU stream).
  operator Stream() const {
    return unwrap();
  }

  /// Get the XPU device type that this stream is associated with.
  DeviceType device_type() const {
    return DeviceType::XPU;
  }

  /// Get the XPU device index that this stream is associated with.
  DeviceIndex device_index() const {
    return stream_.device_index();
  }

  /// Get the full Device that this stream is associated with. The Device is
  /// guaranteed to be a XPU device.
  Device device() const {
    return Device(DeviceType::XPU, device_index());
  }

  /// Return the stream ID corresponding to this particular stream. StreamId is
  /// a int64_t representation generated by its type and index.
  StreamId id() const {
    return stream_.id();
  }

  /// Return true if all enqueued tasks in this stream have been completed,
  /// otherwise return false.
  bool query() const {
    return queue().ext_oneapi_empty();
  }

  /// Performs a blocking wait for the completion of all enqueued tasks in this
  /// stream.
  void synchronize() const {
    queue().wait_and_throw();
    const c10::impl::PyInterpreter* interp = c10::impl::GPUTrace::get_trace();
    if (C10_UNLIKELY(interp)) {
      (*interp)->trace_gpu_stream_synchronization(
          c10::kXPU, reinterpret_cast<uintptr_t>(&queue()));
    }
  }

  /// Return the priority that this stream is associated with. Lower numbers
  /// represent higher priority.
  int priority() const;

  /// Explicit conversion to sycl::queue&.
  sycl::queue& queue() const;

  /// Explicit conversion to Stream.
  Stream unwrap() const {
    return stream_;
  }

  /// Reversibly pack a XPUStream into a struct representation. The XPUStream
  /// can be unpacked using unpack3().
  struct c10::StreamData3 pack3() const {
    return stream_.pack3();
  }

  /// Unpack a XPUStream from the 3 fields generated by pack3().
  static XPUStream unpack3(
      StreamId stream_id,
      DeviceIndex device_index,
      DeviceType device_type) {
    return XPUStream(Stream::unpack3(stream_id, device_index, device_type));
  }

  /// Return the range of priority **supported by PyTorch**.
  static std::tuple<int, int> priority_range() {
    return std::make_tuple(0, -max_compile_time_stream_priorities + 1);
  }

 private:
  Stream stream_;
};

/**
 * Get a stream from the pool in a round-robin fashion.
 *
 * You can request a stream from the highest priority pool by setting
 * isHighPriority to true for a specific device.
 */
C10_XPU_API XPUStream
getStreamFromPool(const bool isHighPriority = false, DeviceIndex device = -1);

/**
 * Get a stream from the pool in a round-robin fashion.
 *
 * You can request a stream by setting a priority value for a specific device.
 * The priority number lower, the priority higher.
 */
C10_XPU_API XPUStream
getStreamFromPool(const int priority, DeviceIndex device = -1);

/**
 * Get an XPUStream from an external SYCL queue.
 *
 * This function allows interoperability with other libraries by enabling
 * the use of an external SYCL queue that was not created by PyTorch. This
 * can be useful for data exchange or other operations where integration
 * with non-PyTorch queues is required.
 *
 * NOTE: It is the user's responsibility to ensure that the referenced SYCL
 * queue remains alive while the corresponding XPUStream, or any c10::Stream
 * derived from it, is in use. The different SYCL queue pointers will result in
 * distinct XPUStream instances, even if the SYCL queues they dereference are
 * equivalent.
 */
C10_XPU_API XPUStream
getStreamFromExternal(sycl::queue* ext_queue, DeviceIndex device_index);

/**
 * Get the current XPU stream, for the passed XPU device, or for the current
 * device if no device index is passed.
 */
C10_XPU_API XPUStream getCurrentXPUStream(DeviceIndex device = -1);

/**
 * Set the current stream on the device of the passed in stream to be the passed
 * in stream.
 */
C10_XPU_API void setCurrentXPUStream(XPUStream stream);

C10_XPU_API std::ostream& operator<<(std::ostream& stream, const XPUStream& s);

/**
 * Block all reserved SYCL queues in the stream pools on the device, and wait
 * for their synchronizations.
 */
C10_XPU_API void syncStreamsOnDevice(DeviceIndex device = -1);

} // namespace c10::xpu

namespace std {
template <>
struct hash<c10::xpu::XPUStream> {
  size_t operator()(c10::xpu::XPUStream s) const noexcept {
    return std::hash<c10::Stream>{}(s.unwrap());
  }
};
} // namespace std
