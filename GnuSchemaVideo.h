#pragma once

#include "GnuSchema.h"

//#import <msdxm.ocx> no_namespace

class CGnuSchemaVideo : public CGnuSchema
{
public:
	CGnuSchemaVideo(void);
	virtual ~CGnuSchemaVideo(void);
	DECLARE_SCHEMA_CLASS()

	virtual void LoadData(SharedFile &File);

	//IMediaPlayerPtr m_Player;

};
