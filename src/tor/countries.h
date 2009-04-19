/****************************************************************************
 ** $Id: torclient.h,v 1.76 2009/01/17 15:49:08 hoganrobert Exp $
 *   Copyright (C) 2006 - 2008 Robert Hogan                                *
 *   robert@roberthogan.net                                                *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA.              *
 ***************************************************************************/
#include <QPixmap>
#include <QString>
#include <QList>

class QPixmap;

class Country
{
public:
    Country( const QString &cc, const QString &name );
    Country( );
    virtual ~Country();

    QPixmap icon() {return m_icon;};
    QString name() {return m_name;};
    QString cc() {return m_cc;};
private:
    QString m_cc;
    QPixmap m_icon;
    QString m_name;
};

class Countries
{
public:
    Countries();
    virtual ~Countries();

    QList<Country*> countryList(){ return countries; };
    Country* country(int offset){ return countries[offset]; };
    int count(){ return countries.count(); };
private:
    typedef QList<Country*> CountryList;
    CountryList countries;
};
