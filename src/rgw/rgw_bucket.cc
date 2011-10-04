#include <errno.h>

#include <string>

#include "common/errno.h"
#include "rgw_access.h"

#include "rgw_bucket.h"
#include "rgw_tools.h"

#include "auth/Crypto.h" // get_random_bytes()

#undef DOUT_CONDVAR
#define DOUT_CONDVAR(cct, x) cct->_conf->rgw_log

static rgw_bucket pi_buckets(BUCKETS_POOL_NAME);

static string avail_pools = ".pools.avail";
static string pool_name_prefix = "p";


int rgw_store_bucket_info(RGWBucketInfo& info)
{
  bufferlist bl;
  ::encode(info, bl);

  string unused;
  int ret = rgw_put_obj(unused, pi_buckets, info.bucket.name, bl.c_str(), bl.length());
  if (ret < 0)
    return ret;

  char bucket_char[16];
  snprintf(bucket_char, sizeof(bucket_char), ".%lld", (long long unsigned)info.bucket.bucket_id);
  string bucket_id_string(bucket_char);
  ret = rgw_put_obj(unused, pi_buckets, bucket_id_string, bl.c_str(), bl.length());

  dout(0) << "rgw_store_bucket_info: bucket=" << info.bucket << " owner " << info.owner << dendl;
  return 0;
}

int rgw_get_bucket_info(string& bucket_name, RGWBucketInfo& info)
{
  bufferlist bl;

  int ret = rgw_get_obj(pi_buckets, bucket_name, bl);
  if (ret < 0) {
    if (ret != -ENOENT)
      return ret;

    info.bucket.name = bucket_name;
    info.bucket.pool = bucket_name; // for now
    return 0;
  }

  bufferlist::iterator iter = bl.begin();
  try {
    ::decode(info, iter);
  } catch (buffer::error& err) {
    dout(0) << "ERROR: could not decode buffer info, caught buffer::error" << dendl;
    return -EIO;
  }

  dout(0) << "rgw_get_bucket_info: bucket=" << info.bucket << " owner " << info.owner << dendl;

  return 0;
}

int rgw_get_bucket_info_id(uint64_t bucket_id, RGWBucketInfo& info)
{
  char bucket_char[16];
  snprintf(bucket_char, sizeof(bucket_char), ".%lld",
           (long long unsigned)bucket_id);
  string bucket_string(bucket_char);

  return rgw_get_bucket_info(bucket_string, info);
}

static int generate_preallocated_pools(vector<string>& pools, int num)
{
  vector<string> names;

  for (int i = 0; i < num; i++) {
    string name = pool_name_prefix;
    append_rand_alpha(pool_name_prefix, name, 8);
    names.push_back(name);
  }
  string uid;
  vector<int> retcodes;
  int ret = rgwstore->create_pools(uid, names, retcodes);
  if (ret < 0)
    return ret;

  vector<int>::iterator riter;
  vector<string>::iterator niter;

  ret = -ENOENT;

  for (riter = retcodes.begin(), niter = names.begin(); riter != retcodes.end(); ++riter, ++niter) {
    int r = *riter;
    if (!r) {
      pools.push_back(*niter);
    } else if (!ret) {
      ret = r;
    }
  }
  if (!pools.size())
    return ret;

  return 0;
}

static int register_available_pools(vector<string>& pools)
{
  map<string, bufferlist> m;
  vector<string>::iterator iter;

  for (iter = pools.begin(); iter != pools.end(); ++iter) {
    bufferlist bl;
    string& name = *iter;
    m[name] = bl;
  }
  rgw_obj obj(pi_buckets, avail_pools);
  int ret = rgwstore->tmap_set(obj, m);
  if (ret == -ENOENT) {
    rgw_bucket new_bucket;
    map<string,bufferlist> attrs;
    string uid;
    ret = rgw_create_bucket(uid, pi_buckets.name, new_bucket, attrs, false);
    if (ret >= 0)
      ret = rgwstore->tmap_set(obj, m);
  }
  if (ret < 0) {
    dout(0) << "rgwstore->tmap_set() failed" << dendl;
    return ret;
  }

  return 0;
}

static int generate_pool(string& bucket_name, rgw_bucket& bucket)
{
  vector<string> pools;
  int ret = generate_preallocated_pools(pools, g_conf->rgw_pools_preallocate_max);
  if (ret < 0) {
    dout(0) << "generate_preallocad_pools returned " << ret << dendl;
    return ret;
  }
  bucket.pool = pools.back();
  bucket.name = bucket_name;

  ret = register_available_pools(pools);
  if (ret < 0) {
    return ret;
  }

  return 0;
}

static int withdraw_pool(string& pool_name)
{
  rgw_obj obj(pi_buckets, avail_pools);
  bufferlist bl;
  return rgwstore->tmap_set(obj, pool_name, bl);
}

int rgw_bucket_maintain_pools()
{
  bufferlist header;
  map<string, bufferlist> m;
  string pool_name;

  rgw_obj obj(pi_buckets, avail_pools);
  int ret = rgwstore->tmap_get(obj, header, m);
  if (ret < 0 && ret != -ENOENT) {
      return ret;
  }

  if ((int)m.size() < g_conf->rgw_pools_preallocate_threshold) {
    dout(0) << "rgw_bucket_maintain_pools allocating pools (m.size()=" << m.size() << " threshold="
               << g_conf->rgw_pools_preallocate_threshold << ")" << dendl;
    vector<string> pools;
    ret = generate_preallocated_pools(pools, g_conf->rgw_pools_preallocate_max - m.size());
    if (ret < 0) {
      dout(0) << "failed to preallocate pools" << dendl;
      return ret;
    }
    ret = register_available_pools(pools);
    if (ret < 0) {
      dout(0) << "failed to register available pools" << dendl;
      return ret;
    }
  }

  return 0;
}

int rgw_bucket_allocate_pool(string& bucket_name, rgw_bucket& bucket)
{
  bufferlist header;
  map<string, bufferlist> m;
  string pool_name;

  rgw_obj obj(pi_buckets, avail_pools);
  int ret = rgwstore->tmap_get(obj, header, m);
  if (ret < 0) {
    if (ret == -ENOENT) {
      return generate_pool(bucket_name, bucket);
    }
    return ret;
  }

  if (!m.size()) {
    return generate_pool(bucket_name, bucket);
  }

  vector<string> v;
  map<string, bufferlist>::iterator miter;
  for (miter = m.begin(); miter != m.end(); ++miter) {
    v.push_back(miter->first);
  }

  uint32_t r;
  ret = get_random_bytes((char *)&r, sizeof(r));
  if (ret < 0)
    return ret;

  int i = r % v.size();
  pool_name = v[i];
  bucket.pool = pool_name;
  bucket.name = bucket_name;
  
  return 0;
}



int rgw_create_bucket(std::string& id, string& bucket_name, rgw_bucket& bucket,
                      map<std::string, bufferlist>& attrs, bool exclusive, uint64_t auid)
{
  /* system bucket name? */
  if (bucket_name[0] == '.') {
    bucket.name = bucket_name;
    bucket.pool = bucket_name;
    return rgwstore->create_bucket(id, bucket, attrs, true, false, exclusive, auid);
  }

  int ret = rgw_bucket_allocate_pool(bucket_name, bucket);
  if (ret < 0)
     return ret;

  ret = rgwstore->create_bucket(id, bucket, attrs, false, true, exclusive, auid);
  if (ret == -EEXIST) {
    return withdraw_pool(bucket.pool);
  }
  if (ret < 0)
    return ret;

  RGWBucketInfo info;
  info.bucket = bucket;
  info.owner = id;
  ret = rgw_store_bucket_info(info);
  if (ret < 0) {
    dout(0) << "failed to store bucket info, removing bucket" << dendl;
    rgwstore->delete_bucket(id, bucket, true);
    return ret;
  }

  return 0; 
}
