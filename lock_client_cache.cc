// RPC stubs for clients to talk to lock_server, and cache the locks
// see lock_client.cache.h for protocol details.

#include "lock_client_cache.h"
#include "rpc.h"
#include <sstream>
#include <iostream>
#include <stdio.h>
#include "tprintf.h"


int lock_client_cache::last_port = 0;

pthread_mutex_t globalClientMutex;
pthread_cond_t gClient_cv;
pthread_cond_t gReleaseThread_cv;

lock_client_cache::lock_client_cache(std::string xdst, 
				     class lock_release_user *_lu)
  : lock_client(xdst), lu(_lu)
{
  rpcs *rlsrpc = new rpcs(0);
  rlsrpc->reg(rlock_protocol::revoke, this, &lock_client_cache::revoke_handler);
  rlsrpc->reg(rlock_protocol::retry, this, &lock_client_cache::retry_handler);

  const char *hname;
  hname = "127.0.0.1";
  std::ostringstream host;
  host << hname << ":" << rlsrpc->port();
  id = host.str();
	VERIFY(pthread_mutex_init(&globalClientMutex, NULL) == 0);
	VERIFY(pthread_cond_init(&gClient_cv, NULL) == 0);
	VERIFY(pthread_cond_init(&gReleaseThread_cv, NULL) == 0);
	pthread_t release_t;		
	pthread_create(&release_t,NULL,lock_client_utility::releaseThread,(void *)this);
}


lock_protocol::status
lock_client_cache::acquire(lock_protocol::lockid_t lid)
{
	pthread_mutex_lock(&globalClientMutex);

start:
	switch (lockMap[lid])
	{
		case acquiring:
			while(lockMap[lid] == acquiring)
			{
				pthread_cond_wait(&gClient_cv, &globalClientMutex);
			}	
			goto start;
			break;
		case releasing:
			while(lockMap[lid] == releasing)
			{
				pthread_cond_wait(&gClient_cv, &globalClientMutex);
			}
			goto start;
			break;
		case none:
			if(lockMetaData[lid].waitingForRetry)
			{
				while(lockMetaData[lid].waitingForRetry)
				{
					pthread_cond_wait(&gClient_cv, &globalClientMutex);
				}
				goto start;
			}
			else
			{
				setRetry(lid);
				if(callAcquire(lid) == lock_protocol::RETRY)
				{
					while(lockMetaData[lid].waitingForRetry)
					{
						pthread_cond_wait(&gClient_cv, &globalClientMutex);
					}
					goto start;
				}
				else
				{
					resetRetry(lid);
				}
			}
			break;
		case free:
			lockMap[lid] = locked;
			break;
		case locked:
			while(lockMap[lid] == locked)
			{
				pthread_cond_wait(&gClient_cv, &globalClientMutex);
			}
			goto start;
			break;
	}
	pthread_cond_broadcast(&gClient_cv);
	pthread_mutex_unlock(&globalClientMutex);
  return lock_protocol::OK;
}

void 
lock_client_cache::setRetry(lock_protocol::lockid_t lid) 
{
	lock_client_Object obj = lockMetaData[lid];
	obj.waitingForRetry = true;
	lockMetaData[lid] = obj;
}

void 
lock_client_cache::resetRetry(lock_protocol::lockid_t lid) 
{
	lock_client_Object obj = lockMetaData[lid];
	obj.waitingForRetry = false;
	lockMetaData[lid] = obj;
}
void 
lock_client_cache::resetFreeNow(lock_protocol::lockid_t lid) 
{
	lock_client_Object obj = lockMetaData[lid];
	obj.freeWhenPossible = false;
	lockMetaData[lid] = obj;
}
	
void 
lock_client_cache::setFreeNow(lock_protocol::lockid_t lid) 
{
	lock_client_Object obj = lockMetaData[lid];
	obj.freeWhenPossible = true;
	lockMetaData[lid] = obj;
}

lock_protocol::status
lock_client_cache::callAcquire(lock_protocol::lockid_t lid) 
{
	lockMap[lid] = acquiring;
	int r;
	tprintf("acquire called \n");
	pthread_mutex_unlock(&globalClientMutex);
	lock_protocol::status ret = cl->call(lock_protocol::acquire, lid,id, r);
	pthread_mutex_lock(&globalClientMutex);
	if(ret == lock_protocol::OK)
	{
		lockMap[lid] = locked;	
	}
	else
	{
		lockMap[lid] = none;
	}
	return ret;
}

lock_protocol::status
lock_client_cache::release(lock_protocol::lockid_t lid) 
{ 
	pthread_mutex_lock(&globalClientMutex);
	lock_protocol::status ret = lock_protocol::OK;
	switch (lockMap[lid])
	{
		case acquiring:
		case releasing:
		case none:
		case free:
			tprintf("release %s for lock %d state %d\n",id.c_str(),lid,lockMap[lid]);
			assert(false);
			break;
		case locked:
			if(lockMetaData[lid].freeWhenPossible)
			{
				resetFreeNow(lid);
				lockMap[lid] = releasing;
				int r;
				tprintf("release called \n");
				pthread_mutex_unlock(&globalClientMutex);
				if(lu != NULL)
				{
					lu->dorelease(lid);
				}
				ret = cl->call(lock_protocol::release, lid,id, r);
				pthread_mutex_lock(&globalClientMutex);

				if(ret == lock_protocol::OK)
				{
					lockMap[lid] = none;
				}
				else
				{
					assert(false);
				}
			}	
			else
			{
				lockMap[lid] = free;
			}
			break;
	}
	pthread_cond_broadcast(&gClient_cv);
	pthread_mutex_unlock(&globalClientMutex);
	return ret;
}

rlock_protocol::status
lock_client_cache::revoke_handler(lock_protocol::lockid_t lid, 
                                  int &r)
{
	pthread_mutex_lock(&globalClientMutex);
	tprintf("revoke %s for lock %d state %d\n",id.c_str(),lid,lockMap[lid]);
	r=0;
  int ret = rlock_protocol::OK;
	switch (lockMap[lid])
	{
		case free:
			{
				lockMap[lid] = releasing;
				toBeReleasedList.push_back(lid);
				pthread_cond_broadcast(&gReleaseThread_cv);
			}
			break;
		case releasing:
		case none:
			assert(false);
			break;
		case acquiring:
		case locked:
			setFreeNow(lid);
			break;
	}
	pthread_mutex_unlock(&globalClientMutex);
  return ret;
}

rlock_protocol::status
lock_client_cache::retry_handler(lock_protocol::lockid_t lid, 
                                 int &)
{
	pthread_mutex_lock(&globalClientMutex);
	int ret = rlock_protocol::OK;
	resetRetry(lid);
	pthread_cond_broadcast(&gClient_cv);
	pthread_mutex_unlock(&globalClientMutex);
  return ret;
}

void* 
lock_client_utility::releaseThread(void *in)
{
	pthread_mutex_lock(&globalClientMutex);
	lock_client_cache *client = (lock_client_cache *)in;

	while(1)
	{
		client->releaseNow();
		pthread_cond_wait(&gReleaseThread_cv, &globalClientMutex);
	}
	pthread_mutex_unlock(&globalClientMutex);
	pthread_exit(NULL);	
}


lock_protocol::status 
lock_client_cache::releaseNow()
{
	lock_protocol::status ret = lock_protocol::OK;
	if(toBeReleasedList.empty())
		return ret;
	while(!toBeReleasedList.empty())
	{
		lock_protocol::lockid_t lid = toBeReleasedList.front();
		if(lockMap[lid] != releasing)
		{
			assert(false);
		}
		resetFreeNow(lid);
		int r;
		tprintf("release called from Thread \n");
		pthread_mutex_unlock(&globalClientMutex);
			if(lu != NULL)
				{
					lu->dorelease(lid);
				}		
		ret = cl->call(lock_protocol::release, lid,id, r);
		pthread_mutex_lock(&globalClientMutex);

		if(ret == lock_protocol::OK)
		{
			lockMap[lid] = none;
			toBeReleasedList.pop_front();
		}
		else
		{
			assert(false);
		}
		pthread_cond_broadcast(&gClient_cv);
	}
	return ret;
}

