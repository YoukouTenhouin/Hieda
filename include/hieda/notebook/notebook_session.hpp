// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>

namespace hieda::notebook {

struct NotebookId {
    std::array<std::byte, 16> bytes{};

    auto operator==(const NotebookId&) const -> bool = default;
    [[nodiscard]] auto toString() const -> std::string;
};

struct NotebookInfo {
    NotebookId id;
    std::filesystem::path path;
    std::uint32_t schemaVersion{0};

    auto operator==(const NotebookInfo&) const -> bool = default;
};

enum class NotebookErrorCode : std::uint8_t {
    pathNotFound,
    pathExists,
    invalidPath,
    invalidNotebook,
    unsupportedVersion,
    alreadyOpen,
    alreadyInUse,
    permissionDenied,
    ioFailure,
};

struct NotebookError {
    NotebookErrorCode code;
    std::filesystem::path path;
    std::string detail;
};

class NotebookException : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

template <typename T> class Result {
  public:
    [[nodiscard]] static auto success(T value) -> Result {
        return Result(std::move(value));
    }
    [[nodiscard]] static auto failure(NotebookError error) -> Result {
        return Result(std::move(error));
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return std::holds_alternative<T>(value_);
    }
    [[nodiscard]] auto value() const& -> const T& {
        return std::get<T>(value_);
    }
    [[nodiscard]] auto value() && -> T {
        return std::get<T>(std::move(value_));
    }
    [[nodiscard]] auto error() const& -> const NotebookError& {
        return std::get<NotebookError>(value_);
    }

  private:
    explicit Result(T value) : value_(std::move(value)) {}
    explicit Result(NotebookError error) : value_(std::move(error)) {}

    std::variant<T, NotebookError> value_;
};

class NotebookSession {
  public:
    NotebookSession();
    ~NotebookSession();

    NotebookSession(const NotebookSession&) = delete;
    auto operator=(const NotebookSession&) -> NotebookSession& = delete;
    NotebookSession(NotebookSession&&) = delete;
    auto operator=(NotebookSession&&) -> NotebookSession& = delete;

    [[nodiscard]] auto create(const std::filesystem::path& path) -> Result<NotebookInfo>;
    [[nodiscard]] auto open(const std::filesystem::path& path) -> Result<NotebookInfo>;
    void close() noexcept;
    [[nodiscard]] auto isOpen() const noexcept -> bool;
    [[nodiscard]] auto current() const -> std::optional<NotebookInfo>;

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace hieda::notebook
