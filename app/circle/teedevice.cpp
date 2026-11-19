// SPDX-License-Identifier: GPL-3.0-or-later

#include "teedevice.h"

CTeeDevice::CTeeDevice (CDevice *pFirst, CDevice *pSecond)
:	m_pFirst (pFirst),
	m_pSecond (pSecond),
	m_bFirstEnabled (TRUE)
{
}

int CTeeDevice::Write (const void *pBuffer, size_t nCount)
{
	int nResult = m_bFirstEnabled ? m_pFirst->Write (pBuffer, nCount) : (int) nCount;
	if (m_pSecond != 0)
	{
		m_pSecond->Write (pBuffer, nCount);
	}
	return nResult;
}
