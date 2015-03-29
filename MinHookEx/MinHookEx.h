#include <unordered_map>
#include <thread>
#include <atomic>

#ifdef _WIN32
#include "minhook/src/HDE/hde32.c"
#elif
#include "minhook/src/HDE/hde64.c"
#endif

#include "minhook/include/MinHook.h"
#include "minhook/src/buffer.c"
#include "minhook/src/trampoline.c"
#include "minhook/src/hook.c"

using namespace std;

class CMinHookEx
{
private:
	class CHook
	{
		friend class CMinHookEx;
	protected:
		LPVOID _pfnTarget;

		CHook(LPVOID pfnTarget) : _pfnTarget(pfnTarget)
		{}

		CHook() : CHook(nullptr)
		{};

		virtual void deleteLater() = 0;		

	public:

		void enable() const
		{
			MH_EnableHook(_pfnTarget);
		}
		void disable() const
		{
			MH_DisableHook(_pfnTarget);
		}
	};

	//------------------------HELPERS-----------------------
#include "Inner/VTableIndex.h"

	template<typename TObj, typename TRet, typename ...TArgs> struct SFunc
	{
		using TFunc = TRet(*)(TArgs...);
	};

	template<typename TObj, typename TRet, typename ...TArgs> struct SFuncSTD : public SFunc < TObj, TRet, TArgs... >
	{
		using TDetour = TRet(__stdcall *)(TObj* pVoid, TArgs...);
	};

	template<typename TObj, typename TRet, typename ...TArgs> struct SFuncCDECL : public SFunc < TObj, TRet, TArgs... >
	{
		using TDetour = TRet(__cdecl *)(TObj* pVoid, TArgs...);
	};

	template<typename TObj, typename TRet, typename ...TArgs> struct SFuncFASTCALL : public SFunc < TObj, TRet, TArgs... >
	{
		using TDetour = TRet(__fastcall *)(TObj* pVoid, TArgs...);
	};

	template<typename TObj, typename TRet, typename ...TArgs>
	static SFuncCDECL<TObj, TRet, TArgs...> funcStripper(TRet(__thiscall TObj::*)(TArgs...))
	{}

	template<typename TObj, typename TRet, typename ...TArgs>
	static SFuncSTD<TObj, TRet, TArgs...> funcStripper(TRet(__stdcall TObj::*)(TArgs...))
	{}

	template<typename TObj, typename TRet, typename ...TArgs>
	static SFuncCDECL<TObj, TRet, TArgs...> funcStripper(TRet(__cdecl TObj::*)(TArgs...))
	{}

	template<typename TObj, typename TRet, typename ...TArgs>
	static SFuncFASTCALL<TObj, TRet, TArgs...> funcStripper(TRet(__fastcall TObj::*)(TArgs...))
	{}

	template<typename TOrigF, typename TObj> struct SMethodPartsH
	{
		using TOrigFunction = TOrigF;
		using TObject = TObj;
		using TFS = decltype(funcStripper<TObj>(declval<TOrigF TObject::*>()));
		using TFunc = typename TFS::TFunc;
		using TDetour = typename TFS::TDetour;
	};

	template<typename TObj, typename TRet, typename ...TArgs>
	static SMethodPartsH<TRet __cdecl(TArgs...), TObj> getMethodParts(TRet(__cdecl TObj::*)(TArgs...)) //fallback for __cdecl methods
	{}																							//no IntelliSense support

	template<typename TOrigFunc, typename TObj>
	static SMethodPartsH<TOrigFunc, TObj> getMethodParts(TOrigFunc TObj::*) //WORKS!
	{}

	template<typename TMethod> struct HelpTypes
	{
	private:
		using TParts = decltype(getMethodParts(declval<TMethod>()));
	public:
		using TOrigFunc = typename TParts::TOrigFunction;
		using TObject = typename TParts::TObject;
		using TFunc = typename TParts::TFunc;
		using TDetour = typename TParts::TDetour;

		template<bool condition, typename TTrueType, typename TFalseType>
		struct ConditionalType
		{};

		template<typename TTrueType, typename TFalseType>
		struct ConditionalType < true, TTrueType, TFalseType >
		{
			using Type = TTrueType;
		};

		template<typename TTrueType, typename TFalseType>
		struct ConditionalType < false, TTrueType, TFalseType >
		{
			using Type = TFalseType;
		};

		template<typename TTrueType, typename TFirstValue>
		static TTrueType conditionalValue(TFirstValue val1)
		{
			return (TTrueType)val1;
		}

		template<typename TTrueType, typename TFirstValue, typename ...TValues> //Unfinished. Needs specialization for the case when no values of TTrueType provided.
		static TTrueType conditionalValue(TFirstValue val1, TValues...Values)
		{
			return is_same<TTrueType, TFirstValue>::value ? (TTrueType)val1 : (TTrueType)conditionalValue<TTrueType>(forward<TValues>(Values)...);
		}

	};

	//---------------------------------------------------------------------------

	template<typename TFunc> class CFunctionHook : public CHook
	{
		friend class CMinHookEx;
	private:
		CFunctionHook(LPVOID pfnTarget, LPVOID pfnOriginal) : CHook(pfnTarget), originalFunc((TFunc*)pfnOriginal)
		{};

		virtual ~CFunctionHook()
		{
			MH_RemoveHook(_pfnTarget);
		}

		virtual void deleteLater() override
		{
			delete this;
		}

	public:
		TFunc* const originalFunc;
	};

	//-------------------------------------------------------------------------
	template<typename TMethod>
	class CMethodHook : public CHook
	{
		friend class CMinHookEx;

	private:
		CMethodHook(LPVOID pfnTarget, typename HelpTypes<TMethod>::TDetour detour) : CHook(pfnTarget), _proxyObject(pfnTarget, detour)
		{};

		virtual void deleteLater() override
		{
			_proxyObject.safeCleanup();
			delete this;
		}

		//------------------------------------------------------------------------
		template<typename TRet, typename ...TArgs>
		class CProxyObject
		{
			friend class CMethodHook < TMethod > ;
		private:
			using TCDECLInvoker = TRet(__cdecl CProxyObject::*)(TArgs...);
			using TInvoker = TRet(CProxyObject::*)(TArgs...);
			using TFASTCALLInvoker = TRet(__fastcall CProxyObject::*)(TArgs...);

			using TTCorCDECL = typename HelpTypes<TMethod>::ConditionalType<is_same<typename HelpTypes<TMethod>::TOrigFunc, TRet __cdecl (TArgs...)>::value, TCDECLInvoker, TInvoker>::Type;

			using TInvokerType = typename HelpTypes<TMethod>::ConditionalType < is_same<typename HelpTypes<TMethod>::TOrigFunc, TRet __fastcall (TArgs...)>::value,
				TFASTCALLInvoker, TTCorCDECL > ::Type;

			TMethod _originalMethod;
			LPVOID _targetMethod;

			CProxyObject(LPVOID target, typename HelpTypes<TMethod>::TDetour detour,
				TInvokerType invoker = HelpTypes<TMethod>::conditionalValue<TInvokerType>(&CProxyObject::invokeOriginalCDECL, &CProxyObject::invokeOriginal, &CProxyObject::invokeOriginalFASTCALL))
				: _targetMethod(target),
					originalMethod((HelpTypes<TMethod>::TOrigFunc*&)invoker) //second arg here to make originalmethod CONST
			{
				LPVOID _pfnOriginal;

				MH_CreateHook(_targetMethod, detourProxyAddr(), &_pfnOriginal);
				
				auto &sk = getKeeper();
				
				sk.detour = detour;
				sk.pOrigM = *(TMethod *)&_pfnOriginal; //Used to call unhooked method via invoker
				sk.threadCounter = 0;
			}

			~CProxyObject()
			{
				delete &getKeeper();
			}

			CProxyObject(const CProxyObject&) = delete;
			CProxyObject& operator=(const CProxyObject&) = delete;

			struct SKeeper
			{
				friend class CMethodHook;
			private:
				unordered_map<thread::id, typename HelpTypes<TMethod>::TObject*> _thisPointers;
				SKeeper() = default;

			public:
				SKeeper(const SKeeper&) = delete;
				SKeeper& operator=(const SKeeper&) = delete;

				typename HelpTypes<TMethod>::TObject* thisPtr()
				{
					return _thisPointers[this_thread::get_id()];
				}

				void setThis(typename HelpTypes<TMethod>::TObject* pThis)
				{
					_thisPointers[this_thread::get_id()] = pThis;
				}

				TMethod pOrigM;
				typename HelpTypes<TMethod>::TDetour detour;
				atomic_uint threadCounter;
			};
			
			static SKeeper& getKeeper()
			{
				static SKeeper *_keeper = new SKeeper();
				return *_keeper;
			}

			TRet invokeOriginal(TArgs...Args)
			{
				auto &sk = getKeeper();
				return (sk.thisPtr()->*(sk.pOrigM))(forward<TArgs>(Args)...);
			}

			TRet __cdecl invokeOriginalCDECL(TArgs...Args)
			{
				return invokeOriginal(forward<TArgs>(Args)...);
			}

			TRet __fastcall invokeOriginalFASTCALL(TArgs...Args)
			{
				return invokeOriginal(forward<TArgs>(Args)...);
			}


			TRet __thiscall invokeDetour(TArgs...Args) //Can't be static because of __thiscall
			{
				auto &sk = getKeeper();
				sk.threadCounter++;
				auto ret = sk.detour((HelpTypes<TMethod>::TObject*)this, forward<TArgs>(Args)...);
				sk.threadCounter--;
				return ret;
			}
			TRet __cdecl invokeDetourCDECL(TArgs...Args)
			{
				auto &sk = getKeeper();
				sk.threadCounter++;
				auto ret = sk.detour((HelpTypes<TMethod>::TObject*)this, forward<TArgs>(Args)...);
				sk.threadCounter--;
				return ret;
			}
			TRet __fastcall invokeDetourFASTCALL(TArgs...Args)
			{
				auto &sk = getKeeper();
				sk.threadCounter++;
				auto ret = sk.detour((HelpTypes<TMethod>::TObject*)this, forward<TArgs>(Args)...);
				sk.threadCounter--;
				return ret;
			}
			TRet __stdcall invokeDetourSTDCALL(TArgs...Args)
			{
				auto &sk = getKeeper();
				sk.threadCounter++;
				auto ret = sk.detour((HelpTypes<TMethod>::TObject*)this, forward<TArgs>(Args)...);
				sk.threadCounter--;
				return ret;
			}

			void safeCleanup()
			{
				MH_DisableHook(_targetMethod);
				auto &sk = getKeeper();
				while(sk.threadCounter > 0)
				{
					this_thread::sleep_for(chrono::milliseconds(10));
				}
				MH_RemoveHook(_targetMethod);
			}

			LPVOID detourProxyAddr()
			{
				auto detourInvoker = &CProxyObject::invokeDetour;
				auto detourInvokerCDECL = &CProxyObject::invokeDetourCDECL;
				auto detourInvokerFASTCALL = &CProxyObject::invokeDetourFASTCALL;
				auto detourInvokerSTDCALL = &CProxyObject::invokeDetourSTDCALL;

				using TO = typename HelpTypes<TMethod>::TObject;

				return is_same<TMethod, TRet(__stdcall TO::*)(TArgs...)>::value ? (LPVOID*&)detourInvokerSTDCALL :
					is_same<TMethod, TRet(__fastcall TO::*)(TArgs...)>::value ? (LPVOID*&)detourInvokerFASTCALL :
					is_same<TMethod, TRet(__cdecl TO::*)(TArgs...)>::value ? (LPVOID*&)detourInvokerCDECL :
					(LPVOID*&)detourInvoker;
			}

		public:
			typename HelpTypes<TMethod>::TOrigFunc* const originalMethod; //user interface; calls proxyObject's invoker
		};

		template<typename TRet, typename ...TArgs>
		static CProxyObject<TRet, TArgs...> proxyObjectH(TRet(TArgs...))
		{}

		using TProxyObject = decltype(proxyObjectH(declval<HelpTypes<TMethod>::TFunc>()));

		TProxyObject _proxyObject;

	public:
		TProxyObject& object(typename HelpTypes<TMethod>::TObject *pThis)
		{
			auto &sk = TProxyObject::getKeeper();
			sk.setThis(pThis);
			return _proxyObject;
		}
	};

	CMinHookEx()
	{
		MH_Initialize();
	}
	CMinHookEx(const CMinHookEx&) = delete;
	CMinHookEx& operator=(const CMinHookEx&) = delete;

	unordered_map<LPVOID, CHook*> _hooks;

public:
	~CMinHookEx()
	{
		for(auto &x : _hooks)
		{
			x.second->deleteLater();
		}
	}

	static CMinHookEx& getInstance()
	{
		static CMinHookEx instance;

		return instance;
	}

	template<typename TMethod>
	CMethodHook<TMethod>& methodHook(TMethod target, typename HelpTypes<TMethod>::TDetour detour)
	{
		auto h = new CMethodHook<TMethod>((LPVOID)(void*&)target, detour);

		_hooks.insert(make_pair((void*&)target, h));

		return *h;
	}

	template<typename TMethod>
	CMethodHook<TMethod>& virtualMethodHook(TMethod target, typename HelpTypes<TMethod>::TObject *object, typename HelpTypes<TMethod>::TDetour detour)
	{
		int vtblOffset = VTableIndex(target);
		int *vtbl = (int*)*(int*)object;
		LPVOID t = (LPVOID)vtbl[vtblOffset];
		auto h = new CMethodHook<TMethod>(t, detour);

		_hooks.insert(make_pair((void*&)target, h));

		return *h;
	}

	template<typename TFunc>
	CFunctionHook<TFunc>& functionHook(TFunc* target, decltype(target) detour)
	{
		LPVOID p;

		MH_CreateHook(target, detour, &p);
		auto h = new CFunctionHook<TFunc>(target, p);

		_hooks.insert(make_pair(target, h));

		return *h;
	}

	template<typename TFunc>
	CFunctionHook<TFunc>* operator[](TFunc *target)
	{
		return (CFunctionHook<TFunc>*)_hooks[(void*&)target];
	}

	template<typename TMethod>
	CMethodHook<TMethod>* operator[](TMethod target)
	{
		return (CMethodHook<TMethod>*)_hooks[(void*&)target];
	}
};