/*
 * miniclient.cpp
 * Copyright (C) 2001, 2002  Justin Karneges
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */

#include <QtCrypto>
#include <QMessageBox>

#include "miniclient.h"
#include "im.h"
#include "proxy.h"
#include "certutil.h"
#include "psiaccount.h"
#include "sslcertdlg.h"
#include "xmpp_tasks.h"

using namespace XMPP;

MiniClient::MiniClient(QObject *parent)
:QObject(parent)
{
	_client = new Client;
	conn = 0;
	tls = 0;
	tlsHandler = 0;
	stream = 0;
	auth = false;
}

MiniClient::~MiniClient()
{
	delete _client;
	reset();
}

void MiniClient::reset()
{
	delete stream;
	stream = 0;

	delete tls;
	tls = 0;
	tlsHandler = 0;

	delete conn;
	conn = 0;
}

void MiniClient::connectToServer(const Jid &jid, bool legacy_ssl_probe, bool ssl, const QString &_host, int _port, ProxyManager *pm, int proxy, QString *_pass)
{
	j = jid;

	QString host;
	int port;
	bool useHost = false;
#ifdef XMPP1
	if(!_host.isEmpty() && !legacy_ssl_probe) {
		useHost = true;
		host = _host;
		port = _port;
	}
#else
	useHost = true;
	if(!_host.isEmpty()) {
		host = _host;
		port = _port;
	}
	else {
		host = jid.host();
		if(ssl)
			port = 5223;
		else
			port = 5222;
	}
#endif

	AdvancedConnector::Proxy p;
	if(proxy > 0) {
		const ProxyItem &pi = pm->getItem(proxy-1);
		if(pi.type == "http") // HTTP Connect
			p.setHttpConnect(pi.settings.host, pi.settings.port);
		else if(pi.type == "socks") // SOCKS
			p.setSocks(pi.settings.host, pi.settings.port);
		else if(pi.type == "poll") { // HTTP Poll
			QUrl u = pi.settings.url;
#ifdef __GNUC__
#warning "This will break with XMPP1"
#endif
			if(u.queryItems().isEmpty()) {
				u.addQueryItem("server",host + ':' + QString::number(port));
			}
	
			p.setHttpPoll(pi.settings.host, pi.settings.port, u.toString());
			p.setPollInterval(2);
		}

		if(pi.settings.useAuth)
			p.setUserPass(pi.settings.user, pi.settings.pass);
	}

	conn = new AdvancedConnector;
	if(ssl || legacy_ssl_probe) {
		tls = new QCA::TLS;
		tls->setTrustedCertificates(CertUtil::allCertificates());
		tlsHandler = new QCATLSHandler(tls);
		connect(tlsHandler, SIGNAL(tlsHandshaken()), SLOT(tls_handshaken()));
	}
	conn->setProxy(p);
	if (useHost) {
		conn->setOptHostPort(host, port);
		conn->setOptSSL(ssl);
	}
#ifdef XMPP1
	conn->setOptProbe(legacy_ssl_probe);
#endif

	stream = new ClientStream(conn, tlsHandler);
#ifndef XMPP1
	stream->setOldOnly(true);
#endif
	connect(stream, SIGNAL(connected()), SLOT(cs_connected()));
	connect(stream, SIGNAL(securityLayerActivated(int)), SLOT(cs_securityLayerActivated(int)));
	connect(stream, SIGNAL(needAuthParams(bool, bool, bool)), SLOT(cs_needAuthParams(bool, bool, bool)));
	connect(stream, SIGNAL(authenticated()), SLOT(cs_authenticated()));
	connect(stream, SIGNAL(connectionClosed()), SLOT(cs_connectionClosed()));
	connect(stream, SIGNAL(delayedCloseFinished()), SLOT(cs_delayedCloseFinished()));
	connect(stream, SIGNAL(warning(int)), SLOT(cs_warning(int)));
	connect(stream, SIGNAL(error(int)), SLOT(cs_error(int)), Qt::QueuedConnection);

	if(_pass) {
		auth = true;
		pass = *_pass;
		_client->connectToServer(stream, j);
	}
	else {
		auth = false;
		_client->connectToServer(stream, j, false);
	}
}

void MiniClient::close()
{
	_client->close();
	reset();
}

Client *MiniClient::client()
{
	return _client;
}

void MiniClient::tls_handshaken()
{
	QCA::Certificate cert = tls->peerCertificateChain().primary();
	int r = tls->peerIdentityResult();
	if(r != QCA::TLS::Valid) {
		QCA::Validity validity =  tls->peerCertificateValidity();
		QString str = CertUtil::resultToString(r,validity);
		while(1) {
			int n = QMessageBox::warning(0,
				tr("Server Authentication"),
				tr("The %1 certificate failed the authenticity test.").arg(j.host()) + '\n' + tr("Reason: %1.").arg(str),
				tr("&Details..."),
				tr("Co&ntinue"),
				tr("&Cancel"), 0, 2);
			if(n == 0) {
				SSLCertDlg::showCert(cert, r, validity);
			}
			else if(n == 1) {
				tlsHandler->continueAfterHandshake();
				break;
			}
			else if(n == 2) {
				close();
				error();
				break;
			}
		}
	}
	else
		tlsHandler->continueAfterHandshake();
}

void MiniClient::cs_connected()
{
}

void MiniClient::cs_securityLayerActivated(int)
{
}

void MiniClient::cs_needAuthParams(bool user, bool password, bool realm)
{
	if(user) 
		stream->setUsername(j.user());
	if(password)
		stream->setPassword(pass);
	if(realm)
		stream->setRealm(j.domain());
	stream->continueAfterParams();
}

void MiniClient::cs_authenticated()
{
	_client->start(j.host(), j.user(), "", "");

#ifdef XMPP1
	if (!stream->old() && auth) {
		JT_Session *j = new JT_Session(_client->rootTask());
		connect(j,SIGNAL(finished()),SLOT(sessionStart_finished()));
		j->go(true);
	}
	else {
		handshaken();
	}
#else
	handshaken();
#endif
}

void MiniClient::sessionStart_finished()
{
	JT_Session *j = (JT_Session*)sender();
	if ( j->success() ) {
		handshaken();
	}
	else {
		cs_error(-1);
	}
}

void MiniClient::cs_connectionClosed()
{
	cs_error(-1);
}

void MiniClient::cs_delayedCloseFinished()
{
}

void MiniClient::cs_warning(int)
{
	stream->continueAfterWarning();
}

void MiniClient::cs_error(int err)
{
	QString str;
	bool reconn;

	PsiAccount::getErrorInfo(err, conn, stream, tlsHandler, &str, &reconn);
	close();

	QMessageBox::critical(0, tr("Server Error"), tr("There was an error communicating with the Jabber server.\nDetails: %1").arg(str));
	error();
}

