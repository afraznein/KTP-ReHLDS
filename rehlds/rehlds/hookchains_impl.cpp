/*
*
*    This program is free software; you can redistribute it and/or modify it
*    under the terms of the GNU General Public License as published by the
*    Free Software Foundation; either version 2 of the License, or (at
*    your option) any later version.
*
*    This program is distributed in the hope that it will be useful, but
*    WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
*    General Public License for more details.
*
*    You should have received a copy of the GNU General Public License
*    along with this program; if not, write to the Free Software Foundation,
*    Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*
*/

#include "precompiled.h"
#include "hookchains_impl.h"

// KTP: head of the intrusive registry list (see hookchains_impl.h).
AbstractHookChainRegistry* AbstractHookChainRegistry::s_registryListHead = nullptr;

AbstractHookChainRegistry::AbstractHookChainRegistry()
{
	Q_memset(m_Hooks, 0, sizeof(m_Hooks));
	Q_memset(m_Priorities, 0, sizeof(m_Priorities));

	m_NumHooks = 0;

	// KTP: link into the global list at construction (static-init time,
	// single-threaded). Cleared as a batch at shutdown via KTP_ClearAllHooks.
	m_nextRegistry = s_registryListHead;
	s_registryListHead = this;
}

// KTP: splice out of the intrusive list — the counterpart to the ctor's link.
// Registry lifetime is process-long ONLY for the statics; MessageManagerImpl
// deletes message-hook registries at runtime, and a freed node left linked here
// is a heap use-after-free the moment KTP_ClearAllHooks walks it at shutdown.
// Single-threaded (game thread), and the list is short, so a linear splice is fine.
AbstractHookChainRegistry::~AbstractHookChainRegistry()
{
	for (AbstractHookChainRegistry** pp = &s_registryListHead; *pp; pp = &(*pp)->m_nextRegistry)
	{
		if (*pp == this)
		{
			*pp = m_nextRegistry;
			break;
		}
	}
	m_nextRegistry = nullptr;
}

// KTP: empty this chain's hook set. Leaves the engine original intact — a
// subsequent callChain finds m_Hooks[0] == NULL and calls only the original.
void AbstractHookChainRegistry::clearHooks()
{
	Q_memset(m_Hooks, 0, sizeof(m_Hooks));
	Q_memset(m_Priorities, 0, sizeof(m_Priorities));
	m_NumHooks = 0;
}

// KTP: engine-side shutdown safety net — see the class comment. Empties every
// registry so no post-dlclose chain call jumps into an unmapped module.
void KTP_ClearAllHooks()
{
	for (AbstractHookChainRegistry* r = AbstractHookChainRegistry::s_registryListHead; r; r = r->m_nextRegistry)
		r->clearHooks();
}

bool AbstractHookChainRegistry::findHook(void* hookFunc) const
{
	for (auto i = 0; i < m_NumHooks; i++) {
		if (m_Hooks[i] == hookFunc)
			return true;
	}

	return false;
}

void AbstractHookChainRegistry::addHook(void* hookFunc, int priority)
{
	if (!hookFunc) {
		Sys_Error("%s: Parameter hookFunc can't be a nullptr", __func__);
	}

	if (findHook(hookFunc)) {
		Sys_Error("%s: The same handler can't be used twice on the hookchain.", __func__);
	}

	for (auto i = 0; i < MAX_HOOKS_IN_CHAIN; i++)
	{
		if (m_Hooks[i] && priority <= m_Priorities[i])
			continue;

		auto swapHookFunc = m_Hooks[i];
		auto swapPriority = m_Priorities[i];

		m_Hooks[i] = hookFunc;
		m_Priorities[i] = priority;

		hookFunc = swapHookFunc;
		priority = swapPriority;
	}

	if (m_NumHooks >= MAX_HOOKS_IN_CHAIN) {
		Sys_Error("%s: MAX_HOOKS_IN_CHAIN limit hit", __func__);
	}

	m_NumHooks++;
}

void AbstractHookChainRegistry::removeHook(void* hookFunc) {

	// erase hook
	for (auto i = 0; i < m_NumHooks; i++)
	{
		if (hookFunc == m_Hooks[i])
		{
			--m_NumHooks;
			if (m_NumHooks != i)
			{
				Q_memmove(&m_Hooks[i], &m_Hooks[i + 1], (m_NumHooks - i) * sizeof(m_Hooks[0]));
				Q_memmove(&m_Priorities[i], &m_Priorities[i + 1], (m_NumHooks - i) * sizeof(m_Priorities[0]));
				m_Hooks[m_NumHooks] = NULL;
			}
			else
				m_Hooks[i] = NULL;

			return;
		}
	}
}

int AbstractHookChainRegistry::getCount() const {
	return m_NumHooks;
}
