/*
 * VK_LAYER_MGO_budget_fake: tiny Vulkan layer that reports a bigger memory budget.
 *
 * Why: Community Shaders (CSX) checks the DXGI memory budget before it switches to
 * its scaled render targets (Render Scale / FSR). Under Proton that budget comes
 * from DXVK, which gets it from VK_EXT_memory_budget, and it's too low, so CSX
 * falls back to native resolution. This layer only rewrites heapBudget of the
 * device-local heaps in vkGetPhysicalDeviceMemoryProperties2. Nothing else.
 * DXVK still caps its own allocations (use dxvk.maxMemoryBudget for that).
 *
 * Environment:
 *   MGO_BUDGET_FAKE_MIB=24000      budget to report in MiB (default: heap size)
 *   MGO_BUDGET_FAKE_OVERSIZE=1     allow MGO_BUDGET_FAKE_MIB above the real heap size
 *   MGO_BUDGET_FAKE_LOG=/tmp/x.log optional log file (first calls only)
 *
 * Build: gcc -shared -fPIC -O2 -o libVkLayer_mgo_budget_fake.so vk_budget_fake.c
 * Use:   VK_ADD_LAYER_PATH=<this folder> VK_LOADER_LAYERS_ENABLE=VK_LAYER_MGO_budget_fake
 */
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXOBJ 32
#define KEY(h) (*(void **)(h))
#define EXPORT __attribute__((visibility("default")))

typedef struct {
   void *key;
   VkInstance handle;
   PFN_vkGetInstanceProcAddr gipa;
   PFN_vkGetPhysicalDeviceMemoryProperties2 props2;
   PFN_vkGetPhysicalDeviceMemoryProperties2 props2khr;
} instrec_t;

typedef struct {
   void *key;
   PFN_vkGetDeviceProcAddr gdpa;
} devrec_t;

static instrec_t g_inst[MAXOBJ];
static devrec_t g_dev[MAXOBJ];
static pthread_mutex_t g_mtx = PTHREAD_MUTEX_INITIALIZER;
static unsigned long g_calls;

static instrec_t *find_inst(void *key)
{
   for (int i = 0; i < MAXOBJ; i++)
      if (g_inst[i].key == key)
         return &g_inst[i];
   return NULL;
}

static devrec_t *find_dev(void *key)
{
   for (int i = 0; i < MAXOBJ; i++)
      if (g_dev[i].key == key)
         return &g_dev[i];
   return NULL;
}

static void fake_budget(VkPhysicalDeviceMemoryProperties2 *p)
{
   VkPhysicalDeviceMemoryBudgetPropertiesEXT *b = NULL;
   for (VkBaseOutStructure *s = (VkBaseOutStructure *)p->pNext; s; s = s->pNext)
      if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT)
         b = (VkPhysicalDeviceMemoryBudgetPropertiesEXT *)s;
   if (!b)
      return;

   const char *mib_s = getenv("MGO_BUDGET_FAKE_MIB");
   VkDeviceSize mib = mib_s ? (VkDeviceSize)strtoull(mib_s, NULL, 10) << 20 : 0;
   const char *os = getenv("MGO_BUDGET_FAKE_OVERSIZE");
   int oversize = os && os[0] == '1';
   const char *logpath = getenv("MGO_BUDGET_FAKE_LOG");

   pthread_mutex_lock(&g_mtx);
   unsigned long n = g_calls++;
   pthread_mutex_unlock(&g_mtx);
   FILE *log = (logpath && n < 3) ? fopen(logpath, "a") : NULL;

   for (uint32_t i = 0; i < p->memoryProperties.memoryHeapCount; i++) {
      const VkMemoryHeap *h = &p->memoryProperties.memoryHeaps[i];
      if (!(h->flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT))
         continue;
      VkDeviceSize old = b->heapBudget[i];
      b->heapBudget[i] = (mib && (oversize || mib < h->size)) ? mib : h->size;
      if (log)
         fprintf(log, "heap %u (%llu MiB): budget %llu -> %llu MiB\n", i, (unsigned long long)(h->size >> 20),
                 (unsigned long long)(old >> 20), (unsigned long long)(b->heapBudget[i] >> 20));
   }
   if (log)
      fclose(log);
}

static VKAPI_ATTR void VKAPI_CALL layer_GetPhysicalDeviceMemoryProperties2(VkPhysicalDevice pd,
                                                                            VkPhysicalDeviceMemoryProperties2 *p)
{
   pthread_mutex_lock(&g_mtx);
   instrec_t *in = find_inst(KEY(pd));
   PFN_vkGetPhysicalDeviceMemoryProperties2 next = in ? in->props2 : NULL;
   pthread_mutex_unlock(&g_mtx);
   if (!next)
      return;
   next(pd, p);
   fake_budget(p);
}

static VKAPI_ATTR void VKAPI_CALL layer_GetPhysicalDeviceMemoryProperties2KHR(VkPhysicalDevice pd,
                                                                               VkPhysicalDeviceMemoryProperties2 *p)
{
   pthread_mutex_lock(&g_mtx);
   instrec_t *in = find_inst(KEY(pd));
   PFN_vkGetPhysicalDeviceMemoryProperties2 next = in ? (in->props2khr ? in->props2khr : in->props2) : NULL;
   pthread_mutex_unlock(&g_mtx);
   if (!next)
      return;
   next(pd, p);
   fake_budget(p);
}

static VKAPI_ATTR VkResult VKAPI_CALL layer_CreateInstance(const VkInstanceCreateInfo *ci,
                                                            const VkAllocationCallbacks *ac, VkInstance *out)
{
   VkLayerInstanceCreateInfo *lci = (VkLayerInstanceCreateInfo *)ci->pNext;
   while (lci && !(lci->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO && lci->function == VK_LAYER_LINK_INFO))
      lci = (VkLayerInstanceCreateInfo *)lci->pNext;
   if (!lci)
      return VK_ERROR_INITIALIZATION_FAILED;

   PFN_vkGetInstanceProcAddr gipa = lci->u.pLayerInfo->pfnNextGetInstanceProcAddr;
   lci->u.pLayerInfo = lci->u.pLayerInfo->pNext;

   PFN_vkCreateInstance create = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
   if (!create)
      return VK_ERROR_INITIALIZATION_FAILED;
   VkResult r = create(ci, ac, out);
   if (r != VK_SUCCESS)
      return r;

   pthread_mutex_lock(&g_mtx);
   for (int i = 0; i < MAXOBJ; i++) {
      if (!g_inst[i].key) {
         g_inst[i].key = KEY(*out);
         g_inst[i].handle = *out;
         g_inst[i].gipa = gipa;
         g_inst[i].props2 =
            (PFN_vkGetPhysicalDeviceMemoryProperties2)gipa(*out, "vkGetPhysicalDeviceMemoryProperties2");
         g_inst[i].props2khr =
            (PFN_vkGetPhysicalDeviceMemoryProperties2)gipa(*out, "vkGetPhysicalDeviceMemoryProperties2KHR");
         break;
      }
   }
   pthread_mutex_unlock(&g_mtx);
   return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL layer_DestroyInstance(VkInstance instance, const VkAllocationCallbacks *ac)
{
   pthread_mutex_lock(&g_mtx);
   instrec_t *in = find_inst(KEY(instance));
   PFN_vkGetInstanceProcAddr gipa = in ? in->gipa : NULL;
   if (in)
      memset(in, 0, sizeof(*in));
   pthread_mutex_unlock(&g_mtx);
   if (!gipa)
      return;
   PFN_vkDestroyInstance next = (PFN_vkDestroyInstance)gipa(instance, "vkDestroyInstance");
   if (next)
      next(instance, ac);
}

static VKAPI_ATTR VkResult VKAPI_CALL layer_CreateDevice(VkPhysicalDevice pd, const VkDeviceCreateInfo *ci,
                                                          const VkAllocationCallbacks *ac, VkDevice *out)
{
   VkLayerDeviceCreateInfo *lci = (VkLayerDeviceCreateInfo *)ci->pNext;
   while (lci && !(lci->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO && lci->function == VK_LAYER_LINK_INFO))
      lci = (VkLayerDeviceCreateInfo *)lci->pNext;
   if (!lci)
      return VK_ERROR_INITIALIZATION_FAILED;

   PFN_vkGetInstanceProcAddr gipa = lci->u.pLayerInfo->pfnNextGetInstanceProcAddr;
   PFN_vkGetDeviceProcAddr gdpa = lci->u.pLayerInfo->pfnNextGetDeviceProcAddr;
   lci->u.pLayerInfo = lci->u.pLayerInfo->pNext;

   pthread_mutex_lock(&g_mtx);
   instrec_t *in = find_inst(KEY(pd));
   VkInstance inst = in ? in->handle : VK_NULL_HANDLE;
   pthread_mutex_unlock(&g_mtx);

   PFN_vkCreateDevice create = (PFN_vkCreateDevice)gipa(inst, "vkCreateDevice");
   if (!create)
      return VK_ERROR_INITIALIZATION_FAILED;
   VkResult r = create(pd, ci, ac, out);
   if (r != VK_SUCCESS)
      return r;

   pthread_mutex_lock(&g_mtx);
   for (int i = 0; i < MAXOBJ; i++) {
      if (!g_dev[i].key) {
         g_dev[i].key = KEY(*out);
         g_dev[i].gdpa = gdpa;
         break;
      }
   }
   pthread_mutex_unlock(&g_mtx);
   return VK_SUCCESS;
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL layer_GetDeviceProcAddr(VkDevice device, const char *name)
{
   if (!strcmp(name, "vkGetDeviceProcAddr"))
      return (PFN_vkVoidFunction)layer_GetDeviceProcAddr;
   pthread_mutex_lock(&g_mtx);
   devrec_t *d = find_dev(KEY(device));
   PFN_vkGetDeviceProcAddr gdpa = d ? d->gdpa : NULL;
   pthread_mutex_unlock(&g_mtx);
   return gdpa ? gdpa(device, name) : NULL;
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL layer_GetInstanceProcAddr(VkInstance instance, const char *name)
{
   if (!strcmp(name, "vkGetInstanceProcAddr"))
      return (PFN_vkVoidFunction)layer_GetInstanceProcAddr;
   if (!strcmp(name, "vkCreateInstance"))
      return (PFN_vkVoidFunction)layer_CreateInstance;
   if (!strcmp(name, "vkDestroyInstance"))
      return (PFN_vkVoidFunction)layer_DestroyInstance;
   if (!strcmp(name, "vkCreateDevice"))
      return (PFN_vkVoidFunction)layer_CreateDevice;
   if (!strcmp(name, "vkGetDeviceProcAddr"))
      return (PFN_vkVoidFunction)layer_GetDeviceProcAddr;
   if (!strcmp(name, "vkGetPhysicalDeviceMemoryProperties2"))
      return (PFN_vkVoidFunction)layer_GetPhysicalDeviceMemoryProperties2;
   if (!strcmp(name, "vkGetPhysicalDeviceMemoryProperties2KHR"))
      return (PFN_vkVoidFunction)layer_GetPhysicalDeviceMemoryProperties2KHR;

   if (!instance)
      return NULL;
   pthread_mutex_lock(&g_mtx);
   instrec_t *in = find_inst(KEY(instance));
   PFN_vkGetInstanceProcAddr gipa = in ? in->gipa : NULL;
   pthread_mutex_unlock(&g_mtx);
   return gipa ? gipa(instance, name) : NULL;
}

EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface *v)
{
   if (v->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT)
      return VK_ERROR_INITIALIZATION_FAILED;
   if (v->loaderLayerInterfaceVersion > 2)
      v->loaderLayerInterfaceVersion = 2;
   v->pfnGetInstanceProcAddr = layer_GetInstanceProcAddr;
   v->pfnGetDeviceProcAddr = layer_GetDeviceProcAddr;
   v->pfnGetPhysicalDeviceProcAddr = NULL;
   return VK_SUCCESS;
}
