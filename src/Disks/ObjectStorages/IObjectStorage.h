#pragma once

#include <filesystem>
#include <string>
#include <map>
#include <mutex>
#include <optional>

#include <Poco/Timestamp.h>
#include <Poco/Util/AbstractConfiguration.h>
#include <Core/Defines.h>
#include <Common/Exception.h>
#include <IO/ReadSettings.h>
#include <IO/WriteSettings.h>

#include <Disks/IO/AsynchronousReadIndirectBufferFromRemoteFS.h>
#include <Disks/ObjectStorages/StoredObject.h>
#include <Disks/DiskType.h>
#include <Common/ThreadPool_fwd.h>
#include <Disks/WriteMode.h>


namespace DB
{

class ReadBufferFromFileBase;
class WriteBufferFromFileBase;

using ObjectAttributes = std::map<std::string, std::string>;

struct RelativePathWithSize
{
    String relative_path;
    size_t bytes_size;

    RelativePathWithSize() = default;

    RelativePathWithSize(const String & relative_path_, size_t bytes_size_)
        : relative_path(relative_path_), bytes_size(bytes_size_) {}
};
using RelativePathsWithSize = std::vector<RelativePathWithSize>;


struct ObjectMetadata
{
    uint64_t size_bytes;
    std::optional<Poco::Timestamp> last_modified;
    std::optional<ObjectAttributes> attributes;
};

using FinalizeCallback = std::function<void(size_t bytes_count)>;

/// Base class for all object storages which implement some subset of ordinary filesystem operations.
///
/// Examples of object storages are S3, Azure Blob Storage, HDFS.
class IObjectStorage
{
public:
    IObjectStorage() = default;

    virtual DataSourceDescription getDataSourceDescription() const = 0;

    virtual std::string getName() const = 0;

    /// Object exists or not
    virtual bool exists(const StoredObject & object) const = 0;

    /// List all objects with specific prefix.
    ///
    /// For example if you do this over filesystem, you should skip folders and
    /// return files only, so something like on local filesystem:
    ///
    ///     find . -type f
    ///
    /// @param children - out files (relative paths) with their sizes.
    /// @param max_keys - return not more then max_keys children
    /// NOTE: max_keys is not the same as list_object_keys_size (disk property)
    /// - if max_keys is set not more then max_keys keys should be returned
    /// - however list_object_keys_size determine the size of the batch and should return all keys
    ///
    /// NOTE: It makes sense only for real object storages (S3, Azure), since
    /// it is used only for one of the following:
    /// - send_metadata (to restore metadata)
    ///   - see DiskObjectStorage::restoreMetadataIfNeeded()
    /// - MetadataStorageFromPlainObjectStorage - only for s3_plain disk
    virtual void findAllFiles(const std::string & path, RelativePathsWithSize & children, int max_keys) const;

    /// Analog of directory content for object storage (object storage does not
    /// have "directory" definition, but it can be emulated with usage of
    /// "delimiter"), so this is analog of:
    ///
    ///     find . -maxdepth 1 $path
    ///
    /// Return files in @files and directories in @directories
    virtual void getDirectoryContents(const std::string & path,
        RelativePathsWithSize & files,
        std::vector<std::string> & directories) const;

    /// Get object metadata if supported. It should be possible to receive
    /// at least size of object
    virtual ObjectMetadata getObjectMetadata(const std::string & path) const = 0;

    /// Read single object
    virtual std::unique_ptr<ReadBufferFromFileBase> readObject( /// NOLINT
        const StoredObject & object,
        const ReadSettings & read_settings = ReadSettings{},
        std::optional<size_t> read_hint = {},
        std::optional<size_t> file_size = {}) const = 0;

    /// Read multiple objects with common prefix
    virtual std::unique_ptr<ReadBufferFromFileBase> readObjects( /// NOLINT
        const StoredObjects & objects,
        const ReadSettings & read_settings = ReadSettings{},
        std::optional<size_t> read_hint = {},
        std::optional<size_t> file_size = {}) const = 0;

    /// Open the file for write and return WriteBufferFromFileBase object.
    virtual std::unique_ptr<WriteBufferFromFileBase> writeObject( /// NOLINT
        const StoredObject & object,
        WriteMode mode,
        std::optional<ObjectAttributes> attributes = {},
        FinalizeCallback && finalize_callback = {},
        size_t buf_size = DBMS_DEFAULT_BUFFER_SIZE,
        const WriteSettings & write_settings = {}) = 0;

    virtual bool isRemote() const = 0;

    /// Remove object. Throws exception if object doesn't exists.
    virtual void removeObject(const StoredObject & object) = 0;

    /// Remove multiple objects. Some object storages can do batch remove in a more
    /// optimal way.
    virtual void removeObjects(const StoredObjects & objects) = 0;

    /// Remove object on path if exists
    virtual void removeObjectIfExists(const StoredObject & object) = 0;

    /// Remove objects on path if exists
    virtual void removeObjectsIfExist(const StoredObjects & object) = 0;

    /// Copy object with different attributes if required
    virtual void copyObject( /// NOLINT
        const StoredObject & object_from,
        const StoredObject & object_to,
        std::optional<ObjectAttributes> object_to_attributes = {}) = 0;

    /// Copy object to another instance of object storage
    /// by default just read the object from source object storage and write
    /// to destination through buffers.
    virtual void copyObjectToAnotherObjectStorage( /// NOLINT
        const StoredObject & object_from,
        const StoredObject & object_to,
        IObjectStorage & object_storage_to,
        std::optional<ObjectAttributes> object_to_attributes = {});

    virtual ~IObjectStorage() = default;

    virtual const std::string & getCacheName() const;

    static ThreadPool & getThreadPoolWriter();

    virtual void shutdown() = 0;

    virtual void startup() = 0;

    /// Apply new settings, in most cases reiniatilize client and some other staff
    virtual void applyNewSettings(
        const Poco::Util::AbstractConfiguration & config,
        const std::string & config_prefix,
        ContextPtr context) = 0;

    /// Sometimes object storages have something similar to chroot or namespace, for example
    /// buckets in S3. If object storage doesn't have any namepaces return empty string.
    virtual String getObjectsNamespace() const = 0;

    /// FIXME: confusing function required for a very specific case. Create new instance of object storage
    /// in different namespace.
    virtual std::unique_ptr<IObjectStorage> cloneObjectStorage(
        const std::string & new_namespace,
        const Poco::Util::AbstractConfiguration & config,
        const std::string & config_prefix, ContextPtr context) = 0;

    /// Generate blob name for passed absolute local path.
    /// Path can be generated either independently or based on `path`.
    virtual std::string generateBlobNameForPath(const std::string & path);

    /// Get unique id for passed absolute path in object storage.
    virtual std::string getUniqueId(const std::string & path) const { return path; }

    /// Remove filesystem cache.
    virtual void removeCacheIfExists(const std::string & /* path */) {}

    virtual bool supportsCache() const { return false; }

    virtual bool isReadOnly() const { return false; }
    virtual bool isWriteOnce() const { return false; }

    virtual bool supportParallelWrite() const { return false; }

    virtual ReadSettings getAdjustedSettingsFromMetadataFile(const ReadSettings & settings, const std::string & /* path */) const { return settings; }

    virtual WriteSettings getAdjustedSettingsFromMetadataFile(const WriteSettings & settings, const std::string & /* path */) const { return settings; }

    virtual ReadSettings patchSettings(const ReadSettings & read_settings) const;

    virtual WriteSettings patchSettings(const WriteSettings & write_settings) const;

protected:
    /// Should be called from implementation of applyNewSettings()
    void applyRemoteThrottlingSettings(ContextPtr context);

private:
    mutable std::mutex throttlers_mutex;
    ThrottlerPtr remote_read_throttler;
    ThrottlerPtr remote_write_throttler;
};

using ObjectStoragePtr = std::shared_ptr<IObjectStorage>;

}
