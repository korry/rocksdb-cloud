// Copyright (c) 2017 Rockset.
#ifndef ROCKSDB_LITE

#ifndef __STDC_FORMAT_MACROS
#define __STDC_FORMAT_MACROS
#endif

#include "rocksdb/db.h"
#include <inttypes.h>
#include "cloud/aws/aws_env.h"
#include "cloud/cloud_env_wrapper.h"
#include "cloud/db_cloud_impl.h"
#include "cloud/filename.h"
#include "cloud/manifest_reader.h"
#include "rocksdb/env.h"
#include "rocksdb/options.h"
#include "rocksdb/persistent_cache.h"
#include "rocksdb/status.h"
#include "rocksdb/table.h"
#include "util/auto_roll_logger.h"
#include "util/file_reader_writer.h"
#include "util/xxhash.h"

namespace rocksdb {

DBCloudImpl::DBCloudImpl(DB* db) : DBCloud(db), cenv_(nullptr) {}

DBCloudImpl::~DBCloudImpl() {
  // Issue a blocking flush so that the latest manifest
  // is made durable in the cloud.
  Flush(FlushOptions());
}

Status DBCloud::Open(const Options& options, const std::string& dbname,
                     const std::string& persistent_cache_path,
                     const uint64_t persistent_cache_size_gb, DBCloud** dbptr,
                     bool read_only) {
  ColumnFamilyOptions cf_options(options);
  std::vector<ColumnFamilyDescriptor> column_families;
  column_families.push_back(
      ColumnFamilyDescriptor(kDefaultColumnFamilyName, cf_options));
  std::vector<ColumnFamilyHandle*> handles;
  DBCloud* dbcloud = nullptr;
  Status s =
      DBCloud::Open(options, dbname, column_families, persistent_cache_path,
                    persistent_cache_size_gb, &handles, &dbcloud, read_only);
  if (s.ok()) {
    assert(handles.size() == 1);
    // i can delete the handle since DBImpl is always holding a reference to
    // default column family
    delete handles[0];
    *dbptr = dbcloud;
  }
  return s;
}

namespace {
Status writeCloudManifest(Env* local_env, CloudManifest* manifest,
                          std::string fname) {
  std::unique_ptr<WritableFile> file;
  Status s = local_env->NewWritableFile(fname, &file, EnvOptions());
  if (!s.ok()) {
    return s;
  }
  return manifest->WriteToLog(unique_ptr<WritableFileWriter>(
      new WritableFileWriter(std::move(file), EnvOptions())));
}

// we map a longer string (uniqueId) into 16-byte string
std::string getNewEpoch(std::string uniqueId) {
  size_t split = uniqueId.size() / 2;
  auto low = uniqueId.substr(0, split);
  auto hi = uniqueId.substr(split);
  uint64_t hash = XXH32(low.data(), low.size(), 0) +
                  (static_cast<uint64_t>(XXH32(hi.data(), hi.size(), 0)) << 32);
  char buf[17];
  snprintf(buf, sizeof buf, "%0" PRIx64, hash);
  return buf;
}
};

Status DBCloud::Open(const Options& opt, const std::string& local_dbname,
                     const std::vector<ColumnFamilyDescriptor>& column_families,
                     const std::string& persistent_cache_path,
                     const uint64_t persistent_cache_size_gb,
                     std::vector<ColumnFamilyHandle*>* handles, DBCloud** dbptr,
                     bool read_only) {
  Status st;
  Options options = opt;

  // Created logger if it is not already pre-created by user.
  if (!options.info_log) {
    CreateLoggerFromOptions(local_dbname, options, &options.info_log);
  }

  st = DBCloudImpl::SanitizeDirectory(options, local_dbname, read_only);
  if (!st.ok()) {
    return st;
  }

  CloudEnvImpl* cenv = static_cast<CloudEnvImpl*>(options.env);
  Env* local_env = cenv->GetBaseEnv();

  if (!read_only) {
    st = DBCloudImpl::MaybeMigrateManifestFile(local_env, local_dbname);
    if (st.ok()) {
      // Init cloud manifest
      st = DBCloudImpl::FetchCloudManifest(options, local_dbname);
    }
    if (st.ok()) {
      // Inits CloudEnvImpl::cloud_manifest_, which will enable us to read files
      // from the cloud
      st = cenv->LoadLocalCloudManifest(local_dbname);
    }
    if (st.ok()) {
      // Rolls the new epoch in CLOUDMANIFEST if it's deemed necessary
      st = DBCloudImpl::MaybeRollNewEpoch(cenv, local_dbname);
    }
    if (!st.ok()) {
      return st;
    }

    // Do the cleanup, but don't fail if the cleanup fails.
    st = cenv->DeleteInvisibleFiles(local_dbname);
    if (!st.ok()) {
      Log(InfoLogLevel::INFO_LEVEL, options.info_log,
          "Failed to delete invisible files: %s", st.ToString().c_str());
      // Ignore the fail
      st = Status::OK();
    }
  }

  // If a persistent cache path is specified, then we set it in the options.
  if (!persistent_cache_path.empty() && persistent_cache_size_gb) {
    // Get existing options. If the persistent cache is already set, then do
    // not make any change. Otherwise, configure it.
    void* bopt = options.table_factory->GetOptions();
    if (bopt != nullptr) {
      BlockBasedTableOptions* tableopt =
          static_cast<BlockBasedTableOptions*>(bopt);
      if (!tableopt->persistent_cache) {
        std::shared_ptr<PersistentCache> pcache;
        st =
            NewPersistentCache(options.env, persistent_cache_path,
                               persistent_cache_size_gb * 1024L * 1024L * 1024L,
                               options.info_log, false, &pcache);
        if (st.ok()) {
          tableopt->persistent_cache = pcache;
          Log(InfoLogLevel::INFO_LEVEL, options.info_log,
              "Created persistent cache %s with size %ld GB",
              persistent_cache_path.c_str(), persistent_cache_size_gb);
        } else {
          Log(InfoLogLevel::INFO_LEVEL, options.info_log,
              "Unable to create persistent cache %s. %s",
              persistent_cache_path.c_str(), st.ToString().c_str());
          return st;
        }
      }
    }
  }
  // We do not want a very large MANIFEST file because the MANIFEST file is
  // uploaded to S3 for every update, so always enable rolling of Manifest file
  options.max_manifest_file_size = DBCloudImpl::max_manifest_file_size;

  DB* db = nullptr;
  std::string dbid;
  if (read_only) {
    st = DB::OpenForReadOnly(options, local_dbname, column_families, handles,
                             &db);
  } else {
    st = DB::Open(options, local_dbname, column_families, handles, &db);
  }

  // now that the database is opened, all file sizes have been verified and we
  // no longer need to verify file sizes for each file that we open. Note that
  // this might have a data race with background compaction, but it's not a big
  // deal, since it's a boolean and it does not impact correctness in any way.
  if (cenv->GetCloudEnvOptions().validate_filesize) {
    *const_cast<bool*>(&cenv->GetCloudEnvOptions().validate_filesize) = false;
  }

  if (st.ok()) {
    DBCloudImpl* cloud = new DBCloudImpl(db);
    *dbptr = cloud;
    db->GetDbIdentity(dbid);
  }
  Log(InfoLogLevel::INFO_LEVEL, options.info_log,
      "Opened cloud db with local dir %s dbid %s. %s", local_dbname.c_str(),
      dbid.c_str(), st.ToString().c_str());
  return st;
}

Status DBCloudImpl::Savepoint() {
  std::string dbid;
  Options default_options = GetOptions();
  Status st = GetDbIdentity(dbid);
  if (!st.ok()) {
    Log(InfoLogLevel::INFO_LEVEL, default_options.info_log,
        "Savepoint could not get dbid %s", st.ToString().c_str());
    return st;
  }
  CloudEnvImpl* cenv = static_cast<CloudEnvImpl*>(GetEnv());

  // If there is no destination bucket, then nothing to do
  if (cenv->GetDestObjectPrefix().empty() ||
      cenv->GetDestBucketPrefix().empty()) {
    Log(InfoLogLevel::INFO_LEVEL, default_options.info_log,
        "Savepoint on cloud dbid %s has no destination bucket, nothing to do.",
        dbid.c_str());
    return st;
  }

  Log(InfoLogLevel::INFO_LEVEL, default_options.info_log,
      "Savepoint on cloud dbid  %s", dbid.c_str());

  // find all sst files in the db
  std::vector<LiveFileMetaData> live_files;
  GetLiveFilesMetaData(&live_files);

  // If an sst file does not exist in the destination path, then remember it
  std::vector<std::string> to_copy;
  for (auto onefile : live_files) {
    auto remapped_fname = cenv->RemapFilename(onefile.name);
    std::string destpath = cenv->GetDestObjectPrefix() + "/" + remapped_fname;
    if (!cenv->ExistsObject(cenv->GetDestBucketPrefix(), destpath).ok()) {
      to_copy.push_back(remapped_fname);
    }
  }

  // copy all files in parallel
  std::atomic<size_t> next_file_meta_idx(0);
  int max_threads = default_options.max_file_opening_threads;

  std::function<void()> load_handlers_func = [&]() {
    while (true) {
      size_t idx = next_file_meta_idx.fetch_add(1);
      if (idx >= to_copy.size()) {
        break;
      }
      auto& onefile = to_copy[idx];
      Status s = cenv->CopyObject(
          cenv->GetSrcBucketPrefix(), cenv->GetSrcObjectPrefix() + "/" + onefile,
          cenv->GetDestBucketPrefix(), cenv->GetDestObjectPrefix() + "/" + onefile);
      if (!s.ok()) {
        Log(InfoLogLevel::INFO_LEVEL, default_options.info_log,
            "Savepoint on cloud dbid  %s error in copying srcbucket %s srcpath "
            "%s dest bucket %d dest path %s. %s",
            dbid.c_str(), cenv->GetSrcBucketPrefix().c_str(),
            cenv->GetSrcObjectPrefix().c_str(),
            cenv->GetDestBucketPrefix().c_str(),
            cenv->GetDestObjectPrefix().c_str(), s.ToString().c_str());
        if (st.ok()) {
          st = s;  // save at least one error
        }
        break;
      }
    }
  };

  if (max_threads <= 1) {
    load_handlers_func();
  } else {
    std::vector<port::Thread> threads;
    for (int i = 0; i < max_threads; i++) {
      threads.emplace_back(load_handlers_func);
    }
    for (auto& t : threads) {
      t.join();
    }
  }
  return st;
}

Status DBCloudImpl::CreateNewIdentityFile(CloudEnv* cenv,
                                          const Options& options,
                                          const std::string& dbid,
                                          const std::string& local_name) {
  const EnvOptions soptions;
  auto tmp_identity_path = local_name + "/IDENTITY.tmp";
  Env* env = cenv->GetBaseEnv();
  Status st;
  {
    unique_ptr<WritableFile> destfile;
    st = env->NewWritableFile(tmp_identity_path, &destfile, soptions);
    if (!st.ok()) {
      Log(InfoLogLevel::ERROR_LEVEL, options.info_log,
          "[db_cloud_impl] Unable to create local IDENTITY file to %s %s",
          tmp_identity_path.c_str(), st.ToString().c_str());
      return st;
    }
    st = destfile->Append(Slice(dbid));
    if (!st.ok()) {
      Log(InfoLogLevel::ERROR_LEVEL, options.info_log,
          "[db_cloud_impl] Unable to write new dbid to local IDENTITY file "
          "%s %s",
          tmp_identity_path.c_str(), st.ToString().c_str());
      return st;
    }
  }
  Log(InfoLogLevel::DEBUG_LEVEL, options.info_log,
      "[db_cloud_impl] Written new dbid %s to %s %s", dbid.c_str(),
      tmp_identity_path.c_str(), st.ToString().c_str());

  // Rename ID file on local filesystem and upload it to dest bucket too
  st = cenv->RenameFile(tmp_identity_path, local_name + "/IDENTITY");
  if (!st.ok()) {
    Log(InfoLogLevel::ERROR_LEVEL, options.info_log,
        "[db_cloud_impl] Unable to rename newly created IDENTITY.tmp "
        " to IDENTITY. %S",
        st.ToString().c_str());
    return st;
  }
  return st;
}

//
// Shall we re-initialize the local dir?
//
Status DBCloudImpl::NeedsReinitialization(CloudEnv* cenv,
                                          const Options& options,
                                          const std::string& local_dir,
                                          bool* do_reinit) {
  Log(InfoLogLevel::INFO_LEVEL, options.info_log,
      "[db_cloud_impl] NeedsReinitialization: "
      "checking local dir %s src bucket %s src path %s "
      "dest bucket %s dest path %s",
      local_dir.c_str(), cenv->GetSrcBucketPrefix().c_str(),
      cenv->GetSrcObjectPrefix().c_str(), cenv->GetDestBucketPrefix().c_str(),
      cenv->GetDestObjectPrefix().c_str());

  // If no buckets are specified, then we cannot reinit anyways
  if (cenv->GetSrcBucketPrefix().empty() &&
      cenv->GetDestBucketPrefix().empty()) {
    Log(InfoLogLevel::INFO_LEVEL, options.info_log,
        "[db_cloud_impl] NeedsReinitialization: "
        "Both src and dest buckets are empty");
    *do_reinit = false;
    return Status::OK();
  }

  // assume that directory does needs reinitialization
  *do_reinit = true;

  // get local env
  Env* env = cenv->GetBaseEnv();

  // Check if local directory exists
  auto st = env->FileExists(local_dir);
  if (!st.ok()) {
    Log(InfoLogLevel::ERROR_LEVEL, options.info_log,
        "[db_cloud_impl] NeedsReinitialization: "
        "failed to access local dir %s: %s",
        local_dir.c_str(), st.ToString().c_str());
    // If the directory is not found, we should create it. In case of an other
    // IO error, we need to fail
    return st.IsNotFound() ? Status::OK() : st;
  }

  // Check if CURRENT file exists
  st = env->FileExists(CurrentFileName(local_dir));
  if (!st.ok()) {
    Log(InfoLogLevel::ERROR_LEVEL, options.info_log,
        "[db_cloud_impl] NeedsReinitialization: "
        "failed to find CURRENT file %s: %s",
        local_dir.c_str(), st.ToString().c_str());
    return st.IsNotFound() ? Status::OK() : st;
  }

  // Read DBID file from local dir
  std::string local_dbid;
  st = ReadFileToString(env, IdentityFileName(local_dir), &local_dbid);
  if (!st.ok()) {
    Log(InfoLogLevel::ERROR_LEVEL, options.info_log,
        "[db_cloud_impl] NeedsReinitialization: "
        "local dir %s unable to read local dbid: %s",
        local_dir.c_str(), st.ToString().c_str());
    return st.IsNotFound() ? Status::OK() : st;
  }
  local_dbid = rtrim_if(trim(local_dbid), '\n');
  auto& src_bucket = cenv->GetSrcBucketPrefix();
  auto& dest_bucket = cenv->GetDestBucketPrefix();

  // We found a dbid in the local dir. Verify that it matches
  // what we found on the cloud.
  std::string src_dbid;
  std::string src_object_path;

  // If a src bucket is specified, then get src dbid
  if (!src_bucket.empty()) {
    st = cenv->GetPathForDbid(src_bucket, local_dbid, &src_object_path);
    if (!st.ok() && !st.IsNotFound()) {
      // Unable to fetch data from S3. Fail Open request.
      Log(InfoLogLevel::ERROR_LEVEL, options.info_log,
          "[db_cloud_impl] NeedsReinitialization: "
          "Local dbid is %s but unable to find src dbid",
          local_dbid.c_str());
      return st;
    }
    Log(InfoLogLevel::INFO_LEVEL, options.info_log,
        "[db_cloud_impl] NeedsReinitialization: "
        "Local dbid is %s and src object path in registry is '%s'",
        local_dbid.c_str(), src_object_path.c_str());

    if (st.ok()) {
      src_object_path = rtrim_if(trim(src_object_path), '/');
    }
    Log(InfoLogLevel::INFO_LEVEL, options.info_log,
        "[db_cloud_impl] NeedsReinitialization: "
        "Local dbid %d configured src path %s src dbid registry",
        local_dbid.c_str(), src_object_path.c_str());
  }
  std::string dest_dbid;
  std::string dest_object_path;

  // If a dest bucket is specified, then get dest dbid
  if (!dest_bucket.empty()) {
    st = cenv->GetPathForDbid(dest_bucket, local_dbid, &dest_object_path);
    if (!st.ok() && !st.IsNotFound()) {
      // Unable to fetch data from S3. Fail Open request.
      Log(InfoLogLevel::ERROR_LEVEL, options.info_log,
          "[db_cloud_impl] NeedsReinitialization: "
          "Local dbid is %s but unable to find dest dbid",
          local_dbid.c_str());
      return st;
    }
    Log(InfoLogLevel::INFO_LEVEL, options.info_log,
        "[db_cloud_impl] NeedsReinitialization: "
        "Local dbid is %s and dest object path in registry is '%s'",
        local_dbid.c_str(), dest_object_path.c_str());

    if (st.ok()) {
      dest_object_path = rtrim_if(trim(dest_object_path), '/');
      std::string dest_specified_path = cenv->GetDestObjectPrefix();
      dest_specified_path = rtrim_if(trim(dest_specified_path), '/');

      // If the registered dest path does not match the one specified in
      // our env, then fail the OpenDB request.
      if (dest_object_path != dest_specified_path) {
        Log(InfoLogLevel::ERROR_LEVEL, options.info_log,
            "[db_cloud_impl] NeedsReinitialization: "
            "Local dbid %s dest path specified in env is %s "
            " but dest path in registry is %s",
            local_dbid.c_str(), cenv->GetDestObjectPrefix().c_str(),
            dest_object_path.c_str());
        return Status::InvalidArgument(
            "[db_cloud_impl] NeedsReinitialization: bad dest path");
      }
    }
    Log(InfoLogLevel::INFO_LEVEL, options.info_log,
        "[db_cloud_impl] NeedsReinitialization: "
        "Local dbid %d configured path %s matches the dest dbid registry",
        local_dbid.c_str(), dest_object_path.c_str());
  }
  // If we found a src_dbid, then it should be a prefix of local_dbid
  if (!src_dbid.empty()) {
    size_t pos = local_dbid.find(src_dbid);
    if (pos == std::string::npos) {
      Log(InfoLogLevel::ERROR_LEVEL, options.info_log,
          "[db_cloud_impl] NeedsReinitialization: "
          "dbid %s in src bucket %s is not a prefix of local dbid %s",
          src_dbid.c_str(), src_bucket.c_str(), local_dbid.c_str());
      return Status::OK();
    }
    Log(InfoLogLevel::INFO_LEVEL, options.info_log,
        "[db_cloud_impl] NeedsReinitialization: "
        "dbid %s in src bucket %s is a prefix of local dbid %s",
        src_dbid.c_str(), src_bucket.c_str(), local_dbid.c_str());

    // If the local dbid is an exact match with the src dbid, then ensure
    // that we cannot run in a 'clone' mode.
    if (local_dbid == src_dbid) {
      Log(InfoLogLevel::INFO_LEVEL, options.info_log,
          "[db_cloud_impl] NeedsReinitialization: "
          "dbid %s in src bucket %s is same as local dbid",
          src_dbid.c_str(), src_bucket.c_str());

      if (!dest_bucket.empty() && src_bucket != dest_bucket) {
        Log(InfoLogLevel::ERROR_LEVEL, options.info_log,
            "[db_cloud_impl] NeedsReinitialization: "
            "local dbid %s in same as src dbid but clone mode specified",
            local_dbid.c_str());
        return Status::OK();
      }
    }
  }

  // If we found a dest_dbid, then it should be a prefix of local_dbid
  if (!dest_dbid.empty()) {
    size_t pos = local_dbid.find(dest_dbid);
    if (pos == std::string::npos) {
      Log(InfoLogLevel::ERROR_LEVEL, options.info_log,
          "[db_cloud_impl] NeedsReinitialization: "
          "dbid %s in dest bucket %s is not a prefix of local dbid %s",
          dest_dbid.c_str(), dest_bucket.c_str(), local_dbid.c_str());
      return Status::OK();
    }
    Log(InfoLogLevel::INFO_LEVEL, options.info_log,
        "[db_cloud_impl] NeedsReinitialization: "
        "dbid %s in dest bucket %s is a prefix of local dbid %s",
        dest_dbid.c_str(), dest_bucket.c_str(), local_dbid.c_str());

    // If the local dbid is an exact match with the destination dbid, then
    // ensure that we are run not in a 'clone' mode.
    if (local_dbid == dest_dbid) {
      Log(InfoLogLevel::DEBUG_LEVEL, options.info_log,
          "[db_cloud_impl] NeedsReinitialization: "
          "dbid %s in dest bucket %s is same as local dbid",
          dest_dbid.c_str(), dest_bucket.c_str());

      if (!src_bucket.empty() && src_bucket != dest_bucket) {
        Log(InfoLogLevel::ERROR_LEVEL, options.info_log,
            "[db_cloud_impl] NeedsReinitialization: "
            "local dbid %s in same as dest dbid but clone mode specified",
            local_dbid.c_str());
        return Status::OK();
      }
    }
  }
  // We found a local dbid but we did not find this dbid mapping in the bucket.
  if (src_object_path.empty() && dest_object_path.empty()) {
    Log(InfoLogLevel::ERROR_LEVEL, options.info_log,
        "[db_cloud_impl] NeedsReinitialization: "
        "local dbid %s does not have a mapping in src bucket "
        "%s or dest bucket %s",
        local_dbid.c_str(), src_bucket.c_str(), dest_bucket.c_str());
    return Status::OK();
  }
  // ID's in the local dir are valid.

  // The DBID of the local dir is compatible with the src and dest buckets.
  // We do not need any re-initialization of local dir.
  *do_reinit = false;
  return Status::OK();
}

//
// Create appropriate files in the clone dir
//
Status DBCloudImpl::SanitizeDirectory(const Options& options,
                                      const std::string& local_name,
                                      bool readonly) {
  EnvOptions soptions;

  CloudEnvImpl* cenv = static_cast<CloudEnvImpl*>(options.env);
  if (cenv->GetCloudType() == CloudType::kNone) {
    // We don't need to SanitizeDirectory()
    return Status::OK();
  }
  if (cenv->GetCloudType() != CloudType::kAws) {
    return Status::NotSupported("We only support AWS for now.");
  }
  // acquire the local env
  Env* env = cenv->GetBaseEnv();

  // Shall we reinitialize the clone dir?
  bool do_reinit = true;
  Status st =
      DBCloudImpl::NeedsReinitialization(cenv, options, local_name, &do_reinit);
  if (!st.ok()) {
    Log(InfoLogLevel::ERROR_LEVEL, options.info_log,
        "[db_cloud_impl] SanitizeDirectory error inspecting dir %s %s",
        local_name.c_str(), st.ToString().c_str());
    return st;
  }

  // If there is no destination bucket, then we need to suck in all sst files
  // from source bucket at db startup time. We do this by setting max_open_files
  // = -1
  if (cenv->GetDestBucketPrefix().empty()) {
    if (options.max_open_files != -1) {
      Log(InfoLogLevel::ERROR_LEVEL, options.info_log,
          "[db_cloud_impl] SanitizeDirectory error.  "
          " No destination bucket specified. Set options.max_open_files = -1 "
          " to copy in all sst files from src bucket %s into local dir %s",
          cenv->GetSrcObjectPrefix().c_str(), local_name.c_str());
      return Status::InvalidArgument(
          "No destination bucket. "
          "Set options.max_open_files = -1");
    }
    if (!cenv->GetCloudEnvOptions().keep_local_sst_files) {
      Log(InfoLogLevel::ERROR_LEVEL, options.info_log,
          "[db_cloud_impl] SanitizeDirectory error.  "
          " No destination bucket specified. Set options.keep_local_sst_files "
          "= true to copy in all sst files from src bucket %s into local dir "
          "%s",
          cenv->GetSrcObjectPrefix().c_str(), local_name.c_str());
      return Status::InvalidArgument(
          "No destination bucket. "
          "Set options.keep_local_sst_files = true");
    }
  }

  if (!do_reinit) {
    Log(InfoLogLevel::INFO_LEVEL, options.info_log,
        "[db_cloud_impl] SanitizeDirectory local directory %s is good",
        local_name.c_str());
    return Status::OK();
  }
  Log(InfoLogLevel::ERROR_LEVEL, options.info_log,
      "[db_cloud_impl] SanitizeDirectory local directory %s cleanup needed",
      local_name.c_str());

  // Delete all local files
  std::vector<Env::FileAttributes> result;
  st = env->GetChildrenFileAttributes(local_name, &result);
  if (!st.ok() && !st.IsNotFound()) {
    return st;
  }
  for (auto file : result) {
    if (file.name == "." || file.name == "..") {
      continue;
    }
    if (file.name.find("LOG") == 0) {  // keep LOG files
      continue;
    }
    std::string pathname = local_name + "/" + file.name;
    st = env->DeleteFile(pathname);
    if (!st.ok()) {
      return st;
    }
    Log(InfoLogLevel::INFO_LEVEL, options.info_log,
        "[db_cloud_impl] SanitizeDirectory cleaned-up: '%s'", pathname.c_str());
  }

  // If directory does not exist, create it
  if (st.IsNotFound()) {
    if (readonly) {
      return st;
    }
    st = env->CreateDirIfMissing(local_name);
  }
  if (!st.ok()) {
    Log(InfoLogLevel::DEBUG_LEVEL, options.info_log,
        "[db_cloud_impl] SanitizeDirectory error opening dir %s %s",
        local_name.c_str(), st.ToString().c_str());
    return st;
  }

  bool dest_equal_src =
      cenv->GetSrcBucketPrefix() == cenv->GetDestBucketPrefix() &&
      cenv->GetSrcObjectPrefix() == cenv->GetDestObjectPrefix();

  bool got_identity_from_dest = false, got_identity_from_src = false;

  // Download IDENTITY, first try destination, then source
  if (!cenv->GetDestBucketPrefix().empty()) {
    // download IDENTITY from dest
    st = cenv->GetObject(cenv->GetDestBucketPrefix(),
                         IdentityFileName(cenv->GetDestObjectPrefix()),
                         IdentityFileName(local_name));
    if (!st.ok() && !st.IsNotFound()) {
      // If there was an error and it's not IsNotFound() we need to bail
      return st;
    }
    got_identity_from_dest = st.ok();
  }
  if (!cenv->GetSrcBucketPrefix().empty() && !dest_equal_src &&
      !got_identity_from_dest) {
    // download IDENTITY from src
    st = cenv->GetObject(cenv->GetSrcBucketPrefix(),
                         IdentityFileName(cenv->GetSrcObjectPrefix()),
                         IdentityFileName(local_name));
    if (!st.ok() && !st.IsNotFound()) {
      // If there was an error and it's not IsNotFound() we need to bail
      return st;
    }
    got_identity_from_src = true;
  }

  if (!got_identity_from_src && !got_identity_from_dest) {
    // There isn't a valid db in either the src or dest bucket.
    // Return with a success code so that a new DB can be created.
    Log(InfoLogLevel::ERROR_LEVEL, options.info_log,
        "[db_cloud_impl] No valid dbs in src bucket %s src path %s "
        "or dest bucket %s dest path %s",
        cenv->GetSrcBucketPrefix().c_str(), cenv->GetSrcObjectPrefix().c_str(),
        cenv->GetDestBucketPrefix().c_str(),
        cenv->GetDestObjectPrefix().c_str());
    return Status::OK();
  }

  if (got_identity_from_src && !dest_equal_src &&
      !cenv->GetDestBucketPrefix().empty()) {
    // If:
    // 1. there is a dest bucket,
    // 2. which is different from src,
    // 3. and there is no IDENTITY in dest bucket,
    // then we are just opening this database as a clone (for the first time).
    // Create a new dbid for this clone.
    std::string src_dbid;
    st = ReadFileToString(env, IdentityFileName(local_name), &src_dbid);
    if (!st.ok()) {
      return st;
    }
    src_dbid = rtrim_if(trim(src_dbid), '\n');

    std::string new_dbid = src_dbid +
                           std::string(CloudEnvImpl::DBID_SEPARATOR) +
                           env->GenerateUniqueId();

    st = CreateNewIdentityFile(cenv, options, new_dbid, local_name);
    if (!st.ok()) {
      return st;
    }
  }

  // create dummy CURRENT file to point to the dummy manifest (cloud env will
  // remap the filename appropriately, this is just to fool the underyling
  // RocksDB)
  {
    unique_ptr<WritableFile> destfile;
    st = env->NewWritableFile(CurrentFileName(local_name), &destfile, soptions);
    if (!st.ok()) {
      Log(InfoLogLevel::ERROR_LEVEL, options.info_log,
          "[db_cloud_impl] Unable to create local CURRENT file to %s %s",
          local_name.c_str(), st.ToString().c_str());
      return st;
    }
    std::string manifestfile =
        "MANIFEST-000001\n";  // CURRENT file needs a newline
    st = destfile->Append(Slice(manifestfile));
    if (!st.ok()) {
      Log(InfoLogLevel::ERROR_LEVEL, options.info_log,
          "[db_cloud_impl] Unable to write local CURRENT file to %s %s",
          local_name.c_str(), st.ToString().c_str());
      return st;
    }
  }
  return Status::OK();
}

Status DBCloudImpl::FetchCloudManifest(const Options& options,
                                       const std::string& local_dbname) {
  CloudEnvImpl* cenv = static_cast<CloudEnvImpl*>(options.env);
  bool dest = !cenv->GetDestBucketPrefix().empty();
  bool src = !cenv->GetSrcBucketPrefix().empty();
  bool dest_equal_src =
      cenv->GetSrcBucketPrefix() == cenv->GetDestBucketPrefix() &&
      cenv->GetSrcObjectPrefix() == cenv->GetDestObjectPrefix();
  std::string cloudmanifest = CloudManifestFile(local_dbname);
  if (!dest && cenv->GetBaseEnv()->FileExists(cloudmanifest).ok()) {
    // nothing to do here, we have our cloud manifest
    return Status::OK();
  }
  // first try to get cloudmanifest from dest
  if (dest) {
    Status st = cenv->GetObject(cenv->GetDestBucketPrefix(),
                                CloudManifestFile(cenv->GetDestObjectPrefix()),
                                cloudmanifest);
    if (!st.ok() && !st.IsNotFound()) {
      // something went wrong, bail out
      return st;
    }
    if (st.ok()) {
      // found it!
      return st;
    }
  }
  // we couldn't get cloud manifest from dest, need to try from src?
  if (src && !dest_equal_src) {
    Status st = cenv->GetObject(cenv->GetSrcBucketPrefix(),
                                CloudManifestFile(cenv->GetSrcObjectPrefix()),
                                cloudmanifest);
    if (!st.ok() && !st.IsNotFound()) {
      // something went wrong, bail out
      return st;
    }
    if (st.ok()) {
      // found it!
      return st;
    }
  }
  // No cloud manifest, create an empty one
  unique_ptr<CloudManifest> manifest;
  CloudManifest::CreateForEmptyDatabase("", &manifest);
  return writeCloudManifest(cenv->GetBaseEnv(), manifest.get(), cloudmanifest);
}

Status DBCloudImpl::MaybeMigrateManifestFile(Env* local_env,
                                             const std::string& local_dbname) {
  std::string manifest_filename;
  auto st = local_env->FileExists(CurrentFileName(local_dbname));
  if (st.IsNotFound()) {
    // No need to migrate
    return Status::OK();
  }
  if (!st.ok()) {
    return st;
  }
  st = ReadFileToString(local_env, CurrentFileName(local_dbname),
                        &manifest_filename);
  if (!st.ok()) {
    return st;
  }
  // Note: This rename is important for migration. If we are just starting on
  // an old database, our local MANIFEST filename will be something like
  // MANIFEST-00001 instead of MANIFEST. If we don't do the rename we'll
  // download MANIFEST file from the cloud, which might not be what we want do
  // to (especially for databases which don't have a destination bucket
  // specified). This piece of code can be removed post-migration.
  manifest_filename = local_dbname + "/" + rtrim_if(manifest_filename, '\n');
  if (local_env->FileExists(manifest_filename).IsNotFound()) {
    // manifest doesn't exist, shrug
    return Status::OK();
  }
  return local_env->RenameFile(manifest_filename, local_dbname + "/MANIFEST");
}

Status DBCloudImpl::MaybeRollNewEpoch(CloudEnvImpl* cenv,
                                      const std::string& local_dbname) {
  auto oldEpoch = cenv->GetCloudManifest()->GetCurrentEpoch().ToString();
  auto st = cenv->GetBaseEnv()->FileExists(
      ManifestFileWithEpoch(local_dbname, oldEpoch));
  if (!st.ok() && !st.IsNotFound()) {
    return st;
  }
  if (st.ok() && !oldEpoch.empty()) {
    // CLOUDMANIFEST points to the manifest we have locally, we don't have to
    // roll the new epoch. This means that nobody has written to the S3 bucket
    // after our last runtime.
    // Note that we have a condition !oldEpoch.empty() here because we still
    // want to roll the epoch when just starting from the old version of the
    // database (which would have oldEpoch == "").
    cenv->GetCloudManifest()->Finalize();
    // Our job here is done
    return Status::OK();
  }
  // Find next file number. We use dummy MANIFEST filename, which should get
  // remapped into the correct MANIFEST filename through CloudManifest.
  // After this call we should also have a local file named
  // MANIFEST-<current_epoch> (unless st.IsNotFound()).
  uint64_t maxFileNumber;
  st = ManifestReader::GetMaxFileNumberFromManifest(
      cenv, local_dbname + "/MANIFEST-000001", &maxFileNumber);
  if (st.IsNotFound()) {
    // This is a new database!
    maxFileNumber = 0;
    st = Status::OK();
  } else if (!st.ok()) {
    return st;
  }
  // roll new epoch
  auto newEpoch = getNewEpoch(cenv->GenerateUniqueId());
  cenv->GetCloudManifest()->AddEpoch(maxFileNumber, newEpoch);
  cenv->GetCloudManifest()->Finalize();
  if (maxFileNumber > 0) {
    // meaning, this is not a new database and we should have
    // ManifestFileWithEpoch(local_dbname, oldEpoch) locally
    st = cenv->GetBaseEnv()->RenameFile(
        ManifestFileWithEpoch(local_dbname, oldEpoch),
        ManifestFileWithEpoch(local_dbname, newEpoch));
    if (!st.ok()) {
      return st;
    }
  }

  if (!cenv->GetDestBucketPrefix().empty()) {
    // upload new manifest, only if we have it (i.e. this is not a new
    // database, indicated by maxFileNumber)
    if (maxFileNumber > 0) {
      st = cenv->PutObject(
          ManifestFileWithEpoch(local_dbname, newEpoch),
          cenv->GetDestBucketPrefix(),
          ManifestFileWithEpoch(cenv->GetDestObjectPrefix(), newEpoch));
    }
    if (st.ok()) {
      // serialize new cloud manifest to a local file
      st = writeCloudManifest(cenv->GetBaseEnv(), cenv->GetCloudManifest(),
                              CloudManifestFile(local_dbname));
    }
    if (st.ok()) {
      // upload new cloud manifest
      st = cenv->PutObject(CloudManifestFile(local_dbname),
                           cenv->GetDestBucketPrefix(),
                           CloudManifestFile(cenv->GetDestObjectPrefix()));
    }
    if (!st.ok()) {
      return st;
    }
  }
  return Status::OK();
}

}  // namespace rocksdb
#endif  // ROCKSDB_LITE
