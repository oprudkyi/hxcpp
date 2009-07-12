#include <hxObject.h>
#include <map>
#include <vector>
#include <set>

#ifdef _WIN32

#define GC_WIN32_THREADS
#include <time.h>

#else

#ifndef INTERNAL_GC
extern "C" {
#include <gc_config_macros.h>
#include <gc_pthread_redirects.h>
//#include "private/gc_priv.h"
}
#endif


#endif

#ifndef INTERNAL_GC
#include <gc.h>
#include <gc_allocator.h>
#else

typedef std::set<hxObject **> RootSet;
static RootSet sgRootSet;
#endif

// On Mac, we need to call GC_INIT before first alloc
static int sNeedGCInit = true;

#define IS_CONST     0x40000000
#define SIZE_MASK    0x3fffffff


static bool sgAllocInit = 0;


struct AllocInfo
{
   void InitHead()
   {
      mSize = 0;
      mFinalizer = 0;
      mNext = 0;
      mPrev = 0;
   }

   inline void Unlink()
   {
      mPrev->mNext = mNext;
      if (mNext)
        mNext->mPrev = mPrev;
      sTotalSize -= (mSize & SIZE_MASK);
      sTotalObjs--;
      if (mFinalizer)
      {
         hxObject *obj = (hxObject *)(this +1);
         mFinalizer(obj);
      }
   }

   inline void Link()
   {
      mNext = sHead.mNext;
      mPrev = &sHead;
      if (mNext)
         mNext->mPrev = this;
      sHead.mNext = this;
      mFinalizer = 0;
      sTotalSize += (mSize & SIZE_MASK);
      sTotalObjs++;
   }

   static AllocInfo *Create(int inSize)
   {
      AllocInfo *result = (AllocInfo *)malloc( inSize + sizeof(AllocInfo) );
      memset(result,0,inSize+sizeof(AllocInfo));
      result->mSize = inSize;
      result->Link();
      return result;
   }

   AllocInfo *Realloc(int inSize)
   {
      AllocInfo *new_info = (AllocInfo *)malloc( inSize + sizeof(AllocInfo) );
      int s = mSize & SIZE_MASK;
      int min_size = std::min( s, inSize );
      memcpy(new_info, this, min_size + sizeof(AllocInfo) ); 
      if (inSize>s)
         memset(((char *)new_info) + s + sizeof(AllocInfo), 0, inSize-s ); 
      new_info->mSize = (mSize & ~SIZE_MASK) | inSize;
      sTotalSize += inSize-s;
      if (new_info->mNext)
         new_info->mNext->mPrev = new_info;
      if (new_info->mPrev)
         new_info->mPrev->mNext = new_info;
      free(this);
      return new_info;
   }
   
   finalizer  mFinalizer;
   AllocInfo  *mNext;
   AllocInfo  *mPrev;
   // We will also use mSize for the mark-bit.
   // If aligment means that there are 4 bytes spare after size, then we
   //  will use those instead.
   // For strings generated by the linker, rather than malloc, this last int
   //  will be 0xffffffff
   int        mSize;

   static AllocInfo sHead;
   static unsigned int sTotalSize;
   static unsigned int sTotalObjs;
};

AllocInfo AllocInfo::sHead;
unsigned int AllocInfo::sTotalSize = 0;
unsigned int AllocInfo::sTotalObjs = 0;

template<typename T>
struct QuickVec
{
	QuickVec() : mPtr(0), mAlloc(0), mSize(0) { } 
	inline void push(T inT)
	{
		if (mSize+1>=mAlloc)
		{
			mAlloc = 10 + (mSize*3/2);
			mPtr = (T *)realloc(mPtr,sizeof(T)*mAlloc);
		}
		mPtr[mSize++]=inT;
	}
	inline T pop()
	{
		return mPtr[--mSize];
	}
	inline bool empty() const { return !mSize; }
	inline int next()
	{
		if (mSize+1>=mAlloc)
		{
			mAlloc = 10 + (mSize*3/2);
			mPtr = (T *)realloc(mPtr,sizeof(T)*mAlloc);
		}
		return mSize++;
	}
	inline int size() const { return mSize; }
	inline T &operator[](int inIndex) { return mPtr[inIndex]; }

	int mAlloc;
	int mSize;
	T *mPtr;
};


typedef QuickVec<AllocInfo *> SparePointers;

#define POOL_SIZE 65
static SparePointers sgSmallPool[POOL_SIZE];



void *hxInternalNew( size_t inSize, bool inIsObj = false )
{
   AllocInfo *data = 0;

   //printf("hxInternalNew %d\n", inSize);
   if (!sgAllocInit)
   {
      sgAllocInit = true;
      AllocInfo::sHead.InitHead();
   }
   // First run, we can't be sure the pool has initialised - but now we can.
   else if (inSize < POOL_SIZE  && 0)
   {
      SparePointers &spares = sgSmallPool[inSize];
      if (!spares.empty())
      {
          data = spares.pop();
          data->Link();
      }
   }

   if (!data)
      data = AllocInfo::Create(inSize);
   else
      memset(data+1,0,inSize);

   return data + 1;
}



void *hxObject::operator new( size_t inSize, bool inContainer )
{
#ifdef INTERNAL_GC
   return hxInternalNew(inSize,true);
#else
#ifdef __APPLE__
   if (sNeedGCInit)
   {
      sNeedGCInit = false;
      GC_no_dls = 1;
      GC_INIT();
   }
#endif
   void *result = inContainer ?  GC_MALLOC(inSize) : GC_MALLOC_ATOMIC(inSize);

   return result;
#endif
}


void *String::operator new( size_t inSize )
{
#ifdef INTERNAL_GC
   return hxInternalNew(inSize,false);
#else
   return GC_MALLOC_ATOMIC(inSize);
#endif
}

void hxGCAddRoot(hxObject **inRoot)
{
#ifdef INTERNAL_GC
   sgRootSet.insert(inRoot);
#else
   GC_add_roots(inRoot,inRoot+1);
#endif
}

void hxGCRemoveRoot(hxObject **inRoot)
{
#ifdef INTERNAL_GC
   sgRootSet.erase(inRoot);
#else
   GC_remove_roots(inRoot,inRoot+1);
#endif
}




void __hxcpp_collect()
{
   #ifdef INTERNAL_GC
   if (!sgAllocInit) return;

   hxMarkClassStatics();
   hxLibMark();

   for(RootSet::iterator i = sgRootSet.begin(); i!=sgRootSet.end(); ++i)
   {
      hxObject *ptr = **i;
      if (ptr)
         hxGCMark(ptr);
   }

   // And sweep ...
   int deleted = 0;
   int retained = 0;
   AllocInfo *data = AllocInfo::sHead.mNext;
   while(data)
   {
      AllocInfo *next = data->mNext;
      int &flags = ( (int *)(data+1) )[-1];
      if ( !(flags & HX_GC_MARKED) )
      {
          data->Unlink();
          if ( (data->mSize & SIZE_MASK) <POOL_SIZE && 0)
          {
               SparePointers &pool = sgSmallPool[data->mSize & SIZE_MASK];
               if (pool.size()<1000)
               {
                  pool.push(data);
                  data = 0;
               }
          }
          if (data)
             free(data);
          deleted++;
      }
      else
      {
           flags ^= HX_GC_MARKED;
           retained++;
      }
      data = next;
   }

   //printf("Objs freed %d/%d)\n", deleted, retained);

   #else
   GC_gcollect();
   #endif
}


#ifndef INTERNAL_GC
static void hxcpp_finalizer(void * obj, void * client_data)
{
   finalizer f = (finalizer)client_data;
   if (f)
      f( (hxObject *)obj );
}
#endif




void hxGCAddFinalizer(hxObject *v, finalizer f)
{
   if (v)
   {
#ifdef INTERNAL_GC
      AllocInfo *data = ((AllocInfo *)v) - 1;
      data->mFinalizer = f;
#else
      GC_register_finalizer(v,hxcpp_finalizer,(void *)f,0,0);
#endif
   }
}


void __RegisterStatic(void *inPtr,int inSize)
{
#ifndef INTERNAL_GC
   GC_add_roots((char *)inPtr, (char *)inPtr + inSize );
#endif
}


void hxGCInit()
{
#ifndef INTERNAL_GC
   if (sNeedGCInit)
   {
      sNeedGCInit = false;
      // We explicitly register all the statics, and there is quite a performance
      //  boost by doing this...

      GC_no_dls = 1;
      GC_INIT();
   }
#endif
}


/*
void __hxcpp_reachable(hxObject *inPtr)
{
   void *ptr = (char *)inPtr;
   bool ok = GC_is_marked((ptr_t)GC_base(ptr));
   wprintf(L"Marked : %d\n",ok);
}
*/

#if defined(_MSC_VER)
struct ThreadData
{
   ThreadFunc func;
	void       *data;
};

unsigned int __stdcall thread_func(void *data)
{
	ThreadData d = *(ThreadData *)data;
	data=0;
	d.func(d.data);
	return 0;
}

#endif

void hxStartThread(ThreadFunc inFunc,void *inUserData)
{
// TODO
#ifndef INTERNAL_GC

#if defined(_MSC_VER)
	ThreadData *data = (ThreadData *)GC_MALLOC( sizeof(ThreadData) );
	data->func = inFunc;
	data->data = inUserData;
   GC_beginthreadex(0,0,thread_func,data,0,0);
#else
   pthread_t result;
   GC_pthread_create(&result,0,inFunc,inUserData);
#endif


#endif
}


void __hxcpp_enable(bool inEnable)
{
#ifndef INTERNAL_GC
   if (inEnable)
      GC_enable();
   else
      GC_disable();
#endif
}

wchar_t *hxNewString(int inLen)
{
#ifdef INTERNAL_GC
   wchar_t *result =  (wchar_t *)hxInternalNew( (inLen+1)*sizeof(wchar_t) );
#else
   wchar_t *result =  (wchar_t *)GC_MALLOC_ATOMIC((inLen+1)*sizeof(wchar_t));
#endif
   result[inLen] = '\0';
   return result;

}

void *hxNewGCBytes(void *inData,int inSize)
{
#ifdef INTERNAL_GC
   void *result =  hxInternalNew(inSize);
#else
   void *result =  GC_MALLOC(inSize);
#endif
   if (inData)
   {
      memcpy(result,inData,inSize);
   }
   return result;
}


void *hxNewGCPrivate(void *inData,int inSize)
{
#ifdef INTERNAL_GC
   void *result =  hxInternalNew(inSize);
#else
   void *result =  GC_MALLOC_ATOMIC(inSize);
#endif
   if (inData)
   {
      memcpy(result,inData,inSize);
   }
   return result;
}


void *hxGCRealloc(void *inData,int inSize)
{
#ifdef INTERNAL_GC
   if (inData==0)
      return hxInternalNew(inSize);

   AllocInfo *data = ((AllocInfo *)(inData) ) -1;

   AllocInfo *new_data = data->Realloc(inSize);

   return new_data+1;
#else
   return GC_REALLOC(inData, inSize );
#endif
}



void hxGCMark(hxObject *inPtr)
{
   int &flags = ((int *)( dynamic_cast<void *>(inPtr)))[-1];
   if ( !(flags & HX_GC_MARKED) )
   {
      flags |= HX_GC_MARKED;
      inPtr->__Mark();
   }
}

void hxGCMarkString(const void *inPtr)
{
   // printf("Mark %S\n", inPtr);
   int &flags = ((int *)(inPtr))[-1];
   if (  !(flags & HX_GC_MARKED) && !(flags & IS_CONST) )
      flags |= HX_GC_MARKED;
}

// --- FieldObject ------------------------------



#ifdef _WIN32
typedef String StringKey;
#else
typedef const String StringKey;
#endif



#ifdef INTERNAL_GC
typedef std::map<StringKey,Dynamic, std::less<StringKey> > FieldMap;
#else
typedef gc_allocator< std::pair<StringKey, Dynamic> > MapAlloc;
typedef std::map<StringKey,Dynamic, std::less<StringKey>, MapAlloc > FieldMap;
#endif

class hxFieldMap : public FieldMap
{
#ifndef INTERNAL_GC
public:
   void *operator new( size_t inSize ) { return GC_MALLOC(inSize); }
   void operator delete( void * ) { }
#endif

};

hxFieldMap *hxFieldMapCreate()
{
	return new hxFieldMap;
}

bool hxFieldMapGet(hxFieldMap *inMap, const String &inName, Dynamic &outValue)
{
	hxFieldMap::iterator i = inMap->find(inName);
	if (i==inMap->end())
		return false;
	outValue = i->second;
	return true;
}

bool hxFieldMapGet(hxFieldMap *inMap, int inID, Dynamic &outValue)
{
	hxFieldMap::iterator i = inMap->find(__hxcpp_field_from_id(inID));
	if (i==inMap->end())
		return false;
	outValue = i->second;
	return true;
}

void hxFieldMapSet(hxFieldMap *inMap, const String &inName, const Dynamic &inValue)
{
	(*inMap)[inName] = inValue;
}

void hxFieldMapAppendFields(hxFieldMap *inMap,Array<String> &outFields)
{
   for(hxFieldMap::const_iterator i = inMap->begin(); i!= inMap->end(); ++i)
      outFields->push(i->first);
}

void hxFieldMapMark(hxFieldMap *inMap)
{
   for(hxFieldMap::const_iterator i = inMap->begin(); i!= inMap->end(); ++i)
   {
      hxGCMarkString(i->first.__s);
      hxObject *ptr = i->second.GetPtr();
      if (ptr)
      {
         int &flags = ( (int *)(dynamic_cast<void *>(ptr)) )[-1];
         if (  !(flags & HX_GC_MARKED) )
         {
            flags |= HX_GC_MARKED;
            ptr->__Mark();
         }
      }
   }
}

// --- Anon -----
//

void hxAnon_obj::Destroy(hxObject *inObj)
{
   hxAnon_obj *obj = dynamic_cast<hxAnon_obj *>(inObj);
	if (obj)
		delete obj->mFields;
}

hxAnon_obj::hxAnon_obj()
{
   mFields = new hxFieldMap;
	#ifdef INTERNAL_GC
	hxGCAddFinalizer(this,Destroy);
	#endif
}

void hxAnon_obj::__Mark()
{
   hxFieldMapMark(mFields);
}



Dynamic hxAnon_obj::__Field(const String &inString)
{
   hxFieldMap::const_iterator f = mFields->find(inString);
   if (f==mFields->end())
      return null();
   return f->second;
}

bool hxAnon_obj::__HasField(const String &inString)
{
   hxFieldMap::const_iterator f = mFields->find(inString);
   return (f!=mFields->end());
}


bool hxAnon_obj::__Remove(String inKey)
{
   hxFieldMap::iterator f = mFields->find(inKey);
	bool found = f!=mFields->end();
	if (found)
	{
		mFields->erase(f);
	}
   return found;
}


Dynamic hxAnon_obj::__SetField(const String &inString,const Dynamic &inValue)
{
   (*mFields)[inString] = inValue;
   return inValue;
}

hxAnon_obj *hxAnon_obj::Add(const String &inName,const Dynamic &inValue)
{
   (*mFields)[inName] = inValue;
   if (inValue.GetPtr())
		inValue.GetPtr()->__SetThis(this);
   return this;
}


String hxAnon_obj::toString()
{
   String result = String(L"{ ",2);
   bool first = true;
   for(hxFieldMap::const_iterator i = mFields->begin(); i!= mFields->end(); ++i)
   {
      if (first)
      {
         result += i->first + String(L"=",1) + (String)(i->second);
         first = false;
      }
      else
         result += String(L", ") + i->first + String(L"=") + (String)(i->second);
   }
   return result + String(L" }",2);
}

void hxAnon_obj::__GetFields(Array<String> &outFields)
{
   for(hxFieldMap::const_iterator i = mFields->begin(); i!= mFields->end(); ++i)
      outFields->push(i->first);
}



