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

#include "CertUpdate.h"

#include "QSmartCard_p.h"

#include <common/Common.h>
#include <common/PinDialog.h>
#include <common/SslCertificate.h>

#include <smartcardpp/helperMacro.h>

#include <QtCore/QDateTime>
#include <QtCore/QDebug>
#include <QtCore/QVariant>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkRequest>
#include <QtNetwork/QNetworkReply>

#include <cstring>

#define HEADER 28

CertUpdate::CertUpdate( QSmartCard *smartcard, QObject *parent )
:	QObject( parent )
,	d( smartcard->d )
,	manager( new QNetworkAccessManager( this ) )
,	step( 0 )
,	serverStep( 0 )
,	authKey( 0 )
,	signKey( 0 )
,	generateKeys( false )
{
	std::memset(challenge, 0, sizeof(challenge));
	std::memset(personCode, 0, sizeof(personCode));
	std::memset(documentNumber, 0, sizeof(documentNumber));
	std::memset(tmpResult, 0, sizeof(tmpResult));
	std::memset(certInfo, 0, sizeof(certInfo));

	d->m.lock();
	d->card->reconnectWithT0();
	SslCertificate cert = smartcard->data().authCert();
	updateUrl = QUrl( cert.type() & SslCertificate::TestType ? "http://demo.digidoc.ee:80/iduuendusproxy/" : "http://www.sk.ee:80/id-kontroll2/usk/" );
	QByteArray c( QByteArray::number( QDateTime::currentDateTime().toTime_t() ) );
	memcpy( (void*)challenge, c, 8 );
}

CertUpdate::~CertUpdate()
{
	delete d->card;
	d->card = 0;
	d->m.unlock();
}

bool CertUpdate::checkUpdateAllowed()
{
	QByteArray result = QByteArray::fromHex( runStep( step ) );
	if ( result.size() != 22 )
		throwError( tr("Server sent wrong count bytes on first step").toUtf8() );
	if ( result.at( 21 ) != 0x00 )
		throwError( tr("update not allowed!").toUtf8() );
	if ( result.at(4) == 0x06 )
		generateKeys = true;
	return true;
}

void CertUpdate::startUpdate()
{
	QByteArray result;
	for( step = 1; step < 36; step++ )
		result = runStep( step, result );
}

void CertUpdate::throwError( const QString &msg )
{ throw std::runtime_error( msg.toStdString() ); }

QByteArray CertUpdate::runStep( int s, QByteArray result )
{
	QCoreApplication::processEvents();
	switch( s )
	{
	case 1: d->card->setSecEnv( 1 ); break;
	case 2:
		{
			std::string id = d->card->readCardID();
			memcpy( (void*)personCode, id.c_str(), id.size() );
			break;
		}
	case 3:
		{
			d->card->selectMF();
			d->card->setSecEnv( 3 );
			d->card->selectDF( 0xEEEE );
			d->card->selectEF( 0x0033 );
			d->card->setSecEnv( 3 );
		}
		break;
	case 4:
		{
			PinDialog *p = 0;
			if ( !d->card->isSecureConnection() && m_pin.isEmpty() )
			{
				p = new PinDialog( PinDialog::Pin1Type, d->t.authCert(), 0, qApp->activeWindow() );
				if( !p->exec() )
					throw std::runtime_error( "" );
				m_pin = p->text();
			} else if ( d->card->isSecureConnection() ) {
				p = new PinDialog( PinDialog::Pin1PinpadType, d->t.authCert(), 0, qApp->activeWindow() );
				p->show();
			}
			d->cmd = QSmartCardPrivate::ValidateInternal;
			d->type = QSmartCardData::Pin1Type;
			d->pin = PinString( m_pin.toLatin1() );
			d->old.clear();
			d->e.exec();
			delete p;
			switch( d->result )
			{
			case QSmartCard::NoError: break;
			case QSmartCard::CancelError: throw std::runtime_error( "" );
			case QSmartCard::ValidateError:
			default: throwError( tr("Wrong PIN1 code.").toUtf8() );
			}
			break;
		}
	case 5: break;
	case 6:
		{
			result = QByteArray::fromHex( queryServer( s, result ).remove( 0, 16 ) );
			if ( result.isEmpty() || ( generateKeys && result.size() != 19) || ( !generateKeys && result.size() != 114 ) )
				throw std::runtime_error( "step6" );
			if ( !generateKeys )
				step = 25;
			break;
		}
	case 7:	break;
	case 8:
		{
			byte tmp[18];
			memcpy( (void*)tmp, result, 18 );
			try {
				ByteVec r = d->card->runCommand( MAKEVECTOR(tmp) );
				result = r.size() ? QByteArray( (char*)&r[0], (int)r.size() ) : "";
			} catch( CardError e ) {
				throw std::runtime_error( "step8 card error: " + e.desc );
			} catch( std::runtime_error e ) {
				qDebug() << "runtime8: " << e.what();
				throw std::runtime_error( "step8 runtime error" );
			}
			if ( result.isEmpty() || result.size() != 33 || result.size() < 22 )
				throw std::runtime_error( "step8" );

			if( result.at(11) == 0x12 )
				authKey = 0x11;
			else
				authKey = 0x12;
			if( result.at(21) == 0x01 )
				signKey = 0x02;
			else
				signKey = 0x01;
			break;
		}
	case 9:
		{
			step10 = QByteArray::fromHex( queryServer( s, result ).remove( 0, 16 ) );
			if ( step10.isEmpty() || step10.size() != 194 )
				throw std::runtime_error( "step10" );
			break;
		}
	case 10: break;
	case 11:
		{
			d->card->selectEF( 0x0013 );
			break;
		}
	case 12:
		{
			byte tmp[96];
			memcpy( (void*)tmp, step10, 96 );
			memset( tmpResult, 0, sizeof(tmpResult));
			QByteArray tmpByte;
			try {
				ByteVec r = d->card->runCommand( MAKEVECTOR(tmp) );
				tmpByte = QByteArray( (char*)&r[0], (int)r.size() );
				memcpy( (void*)tmpResult, tmpByte, 14 );
			} catch( CardError e ) {
				throw std::runtime_error( "step12 card error: " + e.desc );
			} catch( std::runtime_error e ) {
				qDebug() << "runtime12: " << e.what();
				throw std::runtime_error( "step12 runtime error" );
			}
			if ( tmpByte.isEmpty() || tmpByte.size() != 14 )
				throw std::runtime_error( "step12" );
			break;
		}
	case 13:
		{
			byte tmp[96];
			QByteArray tmp13 = step10;
			tmp13.chop( 1 );
			memcpy( (void*)tmp, tmp13.right(96), 96 );
			QByteArray tmpByte;
			try {
				ByteVec r = d->card->runCommand( MAKEVECTOR(tmp) );
				tmpByte = QByteArray( (char*)&r[0], (int)r.size() );
				memcpy( (void*)&tmpResult[14], tmpByte, 14 );
			} catch( CardError e ) {
				throw std::runtime_error( "step13 card error: " + e.desc );
			} catch( std::runtime_error e ) {
				qDebug() << "runtime13: " << e.what();
				throw std::runtime_error( "step13 runtime error" );
			}
			if ( tmpByte.isEmpty() || tmpByte.size() != 14 )
				throw std::runtime_error( "step13" );
			break;
		}
	case 14:
		{
			d->card->selectEF( 0x1000 );
			break;
		}
	case 15:
		{
			try {
				ByteVec r = d->card->cardChallenge();
				memcpy( (void*)&tmpResult[28], QByteArray( (char*)&r[0], (int)r.size() ), 8 );
			} catch ( std::runtime_error &e ) {
				qDebug() << e.what();
			}
			break;
		}
	case 16:
		{
			result = QByteArray::fromHex( queryServer( s, QByteArray( (char*)tmpResult, sizeof( tmpResult ) ) ).remove( 0, 16 ) );
			if ( result.isEmpty() || result.size() != 54 )
				throw std::runtime_error( "step16" );
			break;
		}
	case 17:
		{
			byte tmp[53];
			memcpy( (void*)tmp, result, 53 );
			try {
				ByteVec r = d->card->runCommand( MAKEVECTOR(tmp) );
				result = QByteArray( (char*)&r[0], (int)r.size() );
			} catch( CardError e ) {
				throw std::runtime_error( "step17 card error: " + e.desc );
			} catch( std::runtime_error e ) {
				qDebug() << "runtime17: " << e.what();
				throw std::runtime_error( "step17 runtime error" );
			}
			if ( result.isEmpty() || result.size() != 48 )
				throw std::runtime_error( "step17a" );

			result = QByteArray::fromHex( queryServer( s, result ).remove( 0, 16 ) );
			if ( result.isEmpty() || result.size() != 22 )
				throw std::runtime_error( "step17b" );
			break;
		}
	case 18:
		{
			byte tmp[] = {0x00,0x22,0x41,0xB6,0x02,0x83,0x00};
			try {
				d->card->runCommand( MAKEVECTOR(tmp) );
			} catch( CardError e ) {
				throw std::runtime_error( "step18 card error: " + e.desc );
			} catch( std::runtime_error e ) {
				qDebug() << "runtime18: " << e.what();
				throw std::runtime_error( "step18 runtime error" );
			}
			break;
		}
	case 19:
		{
			byte tmp[] = {0x00,0x22,0x41,0xA4,0x05,0x83,0x03,0x80,authKey,0x00};
			try {
				d->card->runCommand( MAKEVECTOR(tmp) );
			} catch( CardError e ) {
				throw std::runtime_error( "step19 card error: " + e.desc );
			} catch( std::runtime_error e ) {
				qDebug() << "runtime19: " << e.what();
				throw std::runtime_error( "step19 runtime error" );
			}
			break;
		}
	case 20:
		{
			byte tmp[21];
			memcpy( (void*)tmp, result, 21 );
			try {
				ByteVec r = d->card->runCommand( MAKEVECTOR(tmp) );
				result = QByteArray( (char*)&r[0], (int)r.size() );
			} catch( CardError e ) {
				throw std::runtime_error( "step20 card error: " + e.desc );
			} catch( std::runtime_error e ) {
				qDebug() << "runtime20: " << e.what();
				throw std::runtime_error( "step20 runtime error" );
			}
			if ( result.isEmpty() || result.size() != 14 )
				throw std::runtime_error( "step20a" );

			result = QByteArray::fromHex( queryServer( s, result ).remove( 0, 16 ) );
			if ( result.isEmpty() || result.size() != 19 )
				throw std::runtime_error( "step20b" );
			break;
		}
	case 21:
		{
			byte tmp[18];
			memcpy( (void*)tmp, result, 18 );
			try {
				ByteVec r = d->card->runCommand( MAKEVECTOR(tmp) );
				result = QByteArray( (char*)&r[0], (int)r.size() );
			} catch( CardError e ) {
				throw std::runtime_error( "step21 card error: " + e.desc );
			} catch( std::runtime_error e ) {
				qDebug() << "runtime21: " << e.what();
				throw std::runtime_error( "step21 runtime error" );
			}
			if ( result.isEmpty() || result.size() != 147 || result.size() < 134 )
				throw std::runtime_error( "step21a" );
		
			QByteArray tmpResult = result;

			result = QByteArray::fromHex( queryServer( s, result ).remove( 0, 16 ) );
			if ( result.isEmpty() || result.size() != 22 )
				throw std::runtime_error( "step21b" );

			if ((unsigned char)tmpResult.at( 132 ) == 0x34 && (unsigned char)tmpResult.at( 133 ) >= 0x80 )
			{
				runStep( 3 );
				runStep( 4 );
				runStep( 11 );
				runStep( 12 );
				step = 13;
			}
			break;
		}
	case 22:
		{
			byte tmp[] = {0x00,0x22,0x41,0xB6,0x05,0x83,0x03,0x80,signKey,0x00};
			try {
				d->card->runCommand( MAKEVECTOR(tmp) );
			} catch( CardError e ) {
				throw std::runtime_error( "step22 card error: " + e.desc );
			} catch( std::runtime_error e ) {
				qDebug() << "runtime22: " << e.what();
				throw std::runtime_error( "step22 runtime error" );
			}
			break;
		}
	case 23:
		{
			byte tmp[] = {0x00,0x22,0x41,0xA4,0x05,0x83,0x03,0x80,authKey,0x00};
			try {
				d->card->runCommand( MAKEVECTOR(tmp) );
			} catch( CardError e ) {
				throw std::runtime_error( "step23 card error: " + e.desc );
			} catch( std::runtime_error e ) {
				qDebug() << "runtime23: " << e.what();
				throw std::runtime_error( "step23 runtime error" );
			}
			break;
		}
	case 24:
		{
			serverStep = 9;
			byte tmp[21];
			memcpy( (void*)tmp, result, 21 );
			try {
				ByteVec r = d->card->runCommand( MAKEVECTOR(tmp) );
				result = r.size() ? QByteArray( (char*)&r[0], (int)r.size() ) : "";
			} catch( CardError e ) {
				throw std::runtime_error( "step24 card error: " + e.desc );
			} catch( std::runtime_error e ) {
				qDebug() << "runtime24: " << e.what();
				throw std::runtime_error( "step24 runtime error" );
			}
			if ( result.isEmpty() || result.size() != 14 )
				throw std::runtime_error( "step24a" );

			result = QByteArray::fromHex( queryServer( s, result ).remove( 0, 16 ) );
			if ( result.isEmpty() || result.size() != 19 )
				throw std::runtime_error( "step24b" );
			break;
		}
	case 25:
		{
			byte tmp[18];
			memcpy( (void*)tmp, result, 18 );
			try {
				ByteVec r = d->card->runCommand( MAKEVECTOR(tmp) );
				result = QByteArray( (char*)&r[0], (int)r.size() );
			} catch( CardError e ) {
				throw std::runtime_error( "step25 card error: " + e.desc );
			} catch( std::runtime_error e ) {
				qDebug() << "runtime25: " << e.what();
				throw std::runtime_error( "step25 runtime error" );
			}
			if ( result.isEmpty() || result.size() != 147 || result.size() < 134 )
				throw std::runtime_error( "step25a" );

			QByteArray tmpResult = result;

			result = QByteArray::fromHex( queryServer( s, result ).remove( 0, 16 ) );
			if ( result.isEmpty() || result.size() != 114 )
				throw std::runtime_error( "step25b" );

			if ( (unsigned char)tmpResult.at( 132 ) == 0x34 && (unsigned char)tmpResult.at( 133 ) >= 0x80 )
			{
				runStep( 3 );
				runStep( 4 );
				runStep( 11 );
				runStep( 13 );
				runStep( 14 );
				runStep( 15 );
				runStep( 16 );
				runStep( 17 );
				step = 21;
			}
			break;
		}
	case 26:
		runStep( 3 );
		break;
	case 27:
		runStep( 4 );
		break;
	case 28:
		{
			d->card->selectEF( 0xAACE );
			break;
		}
	case 29:
		{
			memcpy( (void*)certInfo[0], result, result.size() );
			char request[] = { 0x4F, 0x4B };
			for ( int i = 1;; i++ )
			{
				result = QByteArray::fromHex( queryServer( s, QByteArray( (char*)&request, 2 ) ).remove( 0, 16 ) );
				if ( result.isEmpty() || ( result.size() != 114 && result.size() != 39 ) )
					throw std::runtime_error( "step29" );
				memcpy( (void*)certInfo[i], result, result.size() );
				if ( result.size() == 39 )
					break;
			}
			break;
		}
	case 30:
		{
			for ( int i = 0; i < 16; i++ )
			{
				byte tmp[113];
				memcpy( (void*)tmp, certInfo[i], 113 );
				try {
					ByteVec r = d->card->runCommand( MAKEVECTOR(tmp) );
					result = QByteArray( (char*)&r[0], (int)r.size() );
				} catch( CardError e ) {
					throw std::runtime_error( "step30 card error: " + e.desc );
				} catch( std::runtime_error e ) {
					qDebug() << "runtime30: " << e.what();
					throw std::runtime_error( "step30 runtime error" );
				}
				
				if ( result.isEmpty() || result.size() != 14 )
					throw std::runtime_error( "step30" );
			}
			break;
		}
	case 31:
		{
			d->card->selectEF( 0xDDCE );
			break;
		}
	case 32:
		{
			for ( int i = 16; i < 32; i++ )
			{
				byte tmp[113];
				memcpy( (void*)tmp, certInfo[i], 113 );
				try {
					ByteVec r = d->card->runCommand( MAKEVECTOR(tmp) );
					result = QByteArray( (char*)&r[0], (int)r.size() );
				} catch( CardError e ) {
					throw std::runtime_error( "step32 card error: " + e.desc );
				} catch( std::runtime_error e ) {
					qDebug() << "runtime32: " << e.what();
					throw std::runtime_error( "step32 runtime error" );
				}
				
				if ( result.isEmpty() || result.size() != 14 )
					throw std::runtime_error( "step32" );
			}
			if ( !generateKeys )
				step = 36;//done
			break;
		}
	case 33:
		{
			d->card->selectEF( 0x0033 );
			break;
		}
	case 34:
		{
			byte tmp[38];
			memcpy( (void*)tmp, certInfo[32], 38 );
			try {
				ByteVec r = d->card->runCommand( MAKEVECTOR(tmp) );
				result = QByteArray( (char*)&r[0], (int)r.size() );
			} catch( CardError e ) {
				throw std::runtime_error( "step34 card error: " + e.desc );
			} catch( std::runtime_error e ) {
				qDebug() << "runtime34: " << e.what();
				throw std::runtime_error( "step34 runtime error" );
			}
			
			if ( result.isEmpty() || result.size() != 14 )
				throw std::runtime_error( "step34" );

			result = queryServer( s, result ).remove( 0, 16 );
			break;
		}
	default:
		result = queryServer( s, result );
	}
	return result;
}

QByteArray CertUpdate::queryServer( int s, QByteArray result )
{
	const char serviceCode[] = { 0x31, 0x00 };
	QByteArray packet;

	serverStep++;

	switch( s )
	{
	case 0:
		{
			std::string id = d->card->readDocumentID();
			memcpy( (void*)documentNumber, id.c_str(), id.size() );

			char query[HEADER+44];
			memset( query, 0, sizeof(query));
			memcpy( (void*)query, serviceCode, 2 );
			memcpy( (void*)&query[2], QByteArray::number( serverStep ), 2 );
			memcpy( (void*)&query[4], QByteArray::number( 44 ), 2 );
			memcpy( (void*)&query[12], documentNumber, 8 );
			memcpy( (void*)&query[28], d->card->readCardID().c_str(), 11 );
			memcpy( (void*)&query[39], documentNumber, 8 );
			packet = QByteArray( (char*)query, sizeof( query ) ).toHex();
			break;
		}
	case 6:
		{
			char query[HEADER+39];
			memset( query, 0, sizeof(query));
			memcpy( (void*)query, serviceCode, 2 );
			memcpy( (void*)&query[2], QByteArray::number( serverStep ), 2 );
			memcpy( (void*)&query[4], QByteArray::number( 39 ), 2 );
			memcpy( (void*)&query[12], documentNumber, 8 );
			memcpy( (void*)&query[20], challenge, 8 );
			memcpy( (void*)&query[28], personCode, 11 );
			const char tmp[] = { char(generateKeys ? 0x01 : 0x00) };
			memcpy( (void*)&query[47], tmp, 1 );

			packet = QByteArray( (char*)query, sizeof( query ) ).toHex();
			break;
		}
	case 9:
		{
			char query[HEADER+33];
			memset( query, 0, sizeof(query));
			memcpy( (void*)query, serviceCode, 2 );
			memcpy( (void*)&query[2], QByteArray::number( serverStep ), 2 );
			memcpy( (void*)&query[4], QByteArray::number( 33 ), 2 );
			memcpy( (void*)&query[12], documentNumber, 8 );
			memcpy( (void*)&query[20], challenge, 8 );
			memcpy( (void*)&query[28], result, 33 );

			packet = QByteArray( (char*)query, sizeof( query ) ).toHex();
			break;
		}
	case 16:
		{
			char query[HEADER+36];
			memset( query, 0, sizeof(query));
			memcpy( (void*)query, serviceCode, 2 );
			memcpy( (void*)&query[2], QByteArray::number( serverStep ), 2 );
			memcpy( (void*)&query[4], QByteArray::number( 36 ), 2 );
			memcpy( (void*)&query[12], documentNumber, 8 );
			memcpy( (void*)&query[20], challenge, 8 );
			memcpy( (void*)&query[28], result, 36 );

			packet = QByteArray( (char*)query, sizeof( query ) ).toHex();
			break;
		}
	case 17:
		{
			char query[HEADER+48];
			memset( query, 0, sizeof(query));
			memcpy( (void*)query, serviceCode, 2 );
			memcpy( (void*)&query[2], QByteArray::number( serverStep ), 2 );
			memcpy( (void*)&query[4], QByteArray::number( 48 ), 2 );
			memcpy( (void*)&query[12], documentNumber, 8 );
			memcpy( (void*)&query[20], challenge, 8 );
			memcpy( (void*)&query[28], result, 48 );

			packet = QByteArray( (char*)query, sizeof( query ) ).toHex();
			break;
		}
	case 20:
	case 24:
		{
			char query[HEADER+14];
			memset( query, 0, sizeof(query));
			memcpy( (void*)query, serviceCode, 2 );
			memcpy( (void*)&query[2], QByteArray::number( serverStep ), 2 );
			memcpy( (void*)&query[4], QByteArray::number( 14 ), 2 );
			memcpy( (void*)&query[12], documentNumber, 8 );
			memcpy( (void*)&query[20], challenge, 8 );
			memcpy( (void*)&query[28], result, 14 );

			packet = QByteArray( (char*)query, sizeof( query ) ).toHex();
			break;
		}
	case 21:
	case 25:
		{
			char query[HEADER+147];
			memset( query, 0, sizeof(query));
			memcpy( (void*)query, serviceCode, 2 );
			memcpy( (void*)&query[2], QByteArray::number( serverStep ), 2 );
			memcpy( (void*)&query[4], QByteArray::number( 147 ), 3 );
			memcpy( (void*)&query[12], documentNumber, 8 );
			memcpy( (void*)&query[20], challenge, 8 );
			memcpy( (void*)&query[28], result, 147 );

			packet = QByteArray( (char*)query, sizeof( query ) ).toHex();
			break;
		}
	case 29:
		{
			char query[HEADER+2];
			memset( query, 0, sizeof(query));
			memcpy( (void*)query, serviceCode, 2 );
			memcpy( (void*)&query[2], QByteArray::number( serverStep ), 2 );
			memcpy( (void*)&query[4], QByteArray::number( 2 ), 2 );
			memcpy( (void*)&query[12], documentNumber, 8 );
			memcpy( (void*)&query[20], challenge, 8 );
			memcpy( (void*)&query[28], result, 2 );

			packet = QByteArray( (char*)query, sizeof( query ) ).toHex();
			break;
		}
	case 34:
		{
			char query[HEADER+14];
			memset( query, 0, sizeof(query));
			memcpy( (void*)query, serviceCode, 2 );
			memcpy( (void*)&query[2], QByteArray::number( serverStep ), 2 );
			memcpy( (void*)&query[4], QByteArray::number( 14 ), 2 );
			memcpy( (void*)&query[12], documentNumber, 8 );
			memcpy( (void*)&query[20], challenge, 8 );
			memcpy( (void*)&query[28], result, 14 );

			packet = QByteArray( (char*)query, sizeof( query ) ).toHex();
			break;
		}
	default:
		return QByteArray();
	}
	qDebug() << "step " << s << " serverStep: " << serverStep << " send: " << packet.toUpper();

	QNetworkRequest req( updateUrl );
	req.setHeader( QNetworkRequest::ContentTypeHeader, "text/plain" );
	req.setHeader( QNetworkRequest::ContentLengthHeader, packet.size() );
	req.setRawHeader( "User-Agent",
		QString( "%1/%2 (%3)").arg( qApp->applicationName(), qApp->applicationVersion(), Common::applicationOs() ).toUtf8() );
	req.setRawHeader( "Connection", "close" );

	QEventLoop e;
	QNetworkReply *reply = manager->post( req, packet.toUpper() );
	connect( reply, SIGNAL(finished()), &e, SLOT(quit()) );
	connect( reply, SIGNAL(error(QNetworkReply::NetworkError)), &e, SLOT(quit()) );
	e.exec();

	if ( reply->error() != QNetworkReply::NoError )
	{
		qDebug() << reply->errorString();
		reply->deleteLater();
		throwError( tr("Check internet connection").toUtf8() );
		return result;
	}

	result = reply->readAll();
	reply->deleteLater();
	qDebug() << "step " << s << " serverStep: " << serverStep << " receive: " << result;

	//veakoodide kontroll
	QByteArray hex = QByteArray::fromHex( result );
	if ( hex.size() > 4 )
	{
		switch( hex.at(4) )
		{
			case 0x01: throw std::runtime_error( tr( "Server sai vale arvu baite, samm: %1" ).arg( s ).toStdString() );
			case 0x02:
			case 0x03:
			case 0x07:
			case 0x08: throw std::runtime_error( tr( "Serveri töös tekkisid vead, samm: %1" ).arg( s ).toStdString() );
			case 0x04: throw std::runtime_error( tr( "Kaardi vastuse parsimisel tekkis viga, samm: %1" ).arg( s ).toStdString() );
			case 0x05: throw std::runtime_error( tr( "Sertifitseerimiskeskus ei vasta, samm: %1" ).arg( s ).toStdString() );
		}
	}
	return result;
}
