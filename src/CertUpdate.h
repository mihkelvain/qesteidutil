/*
 * QEstEidUtil
 *
 * Copyright (C) 2009-2013 Jargo Kõster <jargo@innovaatik.ee>
 * Copyright (C) 2009-2013 Raul Metsma <raul@innovaatik.ee>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#pragma once

#include <QtCore/QObject>
#include <QtCore/QUrl>

class QNetworkAccessManager;
class QSmartCard;
class QSmartCardPrivate;

class CertUpdate: public QObject
{
	Q_OBJECT

public:

	CertUpdate( QSmartCard *smartcard, QObject *parent = 0 );
	~CertUpdate();

	bool checkUpdateAllowed();
	void startUpdate();

private:
	void throwError( const QString &msg );

	QSmartCardPrivate *d;
	QString m_pin;
	QNetworkAccessManager *manager;
	char challenge[8];
	char personCode[11], documentNumber[8], tmpResult[36], certInfo[33][114];
	int step, serverStep;
	unsigned char authKey, signKey;
	bool generateKeys;
	QByteArray step10;
	
	QByteArray runStep( int step, QByteArray result = "" );
	QByteArray queryServer( int step, QByteArray result );
	QUrl updateUrl;
};
