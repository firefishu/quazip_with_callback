/*
Copyright (C) 2010 Roberto Pompermaier
Copyright (C) 2005-2014 Sergey A. Tachenov

This file is part of QuaZip.

QuaZip is free software: you can redistribute it and/or modify
it under the terms of the GNU Lesser General Public License as published by
the Free Software Foundation, either version 2.1 of the License, or
(at your option) any later version.

QuaZip is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public License
along with QuaZip.  If not, see <http://www.gnu.org/licenses/>.

See COPYING file for the full LGPL text.

Original ZIP package is copyrighted by Gilles Vollant and contributors,
see quazip/(un)zip.h files for details. Basically it's the zlib license.
*/

#include "JlCompress.h"
#define CALLBACK_SIZE 409600
bool JlCompress::copyData(QIODevice &inFile, QIODevice &outFile, const QuazipProgressCallback& callback)
{
    qint64 readPos = 0;
    while (!inFile.atEnd()) {
        char buf[4096];
        qint64 readLen = inFile.read(buf, 4096);
        if (readLen <= 0)
            return false;
        if (outFile.write(buf, readLen) != readLen)
            return false;
        readPos += readLen;
        if (readPos > CALLBACK_SIZE) {
            readPos = 0;
            if (callback && callback((qreal)inFile.pos() / (qreal)inFile.size())) {
                return false;
            }
        }

    }

    if (callback && callback(1)) {
        return false;
    }

    return true;
}

bool JlCompress::compressFile(QuaZip* zip, QString fileName, QString fileDest, const QuazipProgressCallback& callback) {
    // zip: oggetto dove aggiungere il file
    // fileName: nome del file reale
    // fileDest: nome del file all'interno del file compresso

    // Controllo l'apertura dello zip
    if (!zip) return false;
    if (zip->getMode()!=QuaZip::mdCreate &&
        zip->getMode()!=QuaZip::mdAppend &&
        zip->getMode()!=QuaZip::mdAdd) return false;

    // Apro il file risulato
    QuaZipFile outFile(zip);
    if(!outFile.open(QIODevice::WriteOnly, QuaZipNewInfo(fileDest, fileName))) return false;

    bool result = true;
    QFileInfo input(fileName);
    if (quazip_is_symlink(input)) {
        // Not sure if we should use any specialized codecs here.
        // After all, a symlink IS just a byte array. And
        // this is mostly for Linux, where UTF-8 is ubiquitous these days.
        QString path = quazip_symlink_target(input);
        QString relativePath = input.dir().relativeFilePath(path);
        outFile.write(QFile::encodeName(relativePath));
        if (callback) {
            result = !callback(1);
        }
    } else {
        QFile inFile;
        inFile.setFileName(fileName);
        if (!inFile.open(QIODevice::ReadOnly))
            return false;
        if (!copyData(inFile, outFile, callback) || outFile.getZipError()!=UNZ_OK)
            return false;
        inFile.close();
    }

    // Chiudo i file
    outFile.close();
    return result && outFile.getZipError() == UNZ_OK;
}

bool JlCompress::compressSubDir(QuaZip* zip, QString dir, QString origDir, bool recursive, QDir::Filters filters, const QuazipProgressCallback& callback) {
    // zip: oggetto dove aggiungere il file
    // dir: cartella reale corrente
    // origDir: cartella reale originale
    // (path(dir)-path(origDir)) = path interno all'oggetto zip

    // Controllo l'apertura dello zip
    if (!zip) return false;
    if (zip->getMode()!=QuaZip::mdCreate &&
        zip->getMode()!=QuaZip::mdAppend &&
        zip->getMode()!=QuaZip::mdAdd) return false;

    // Controllo la cartella
    QDir directory(dir);
    if (!directory.exists()) return false;

    QDir origDirectory(origDir);
	if (dir != origDir) {
		QuaZipFile dirZipFile(zip);
		if (!dirZipFile.open(QIODevice::WriteOnly,
            QuaZipNewInfo(origDirectory.relativeFilePath(dir) + QLatin1String("/"), dir), nullptr, 0, 0)) {
				return false;
		}
		dirZipFile.close();
	}

    float count = 0.f;
    auto total = (float)directory.entryInfoList(QDir::Files|QDir::AllDirs|QDir::NoDotAndDotDot|filters).size();
    // Se comprimo anche le sotto cartelle
    if (recursive) {
        // Per ogni sotto cartella
        QFileInfoList files = directory.entryInfoList(QDir::AllDirs|QDir::NoDotAndDotDot|filters);
        for (const auto& file : files) {
            if (!file.isDir()) {// needed for Qt < 4.7 because it doesn't understand AllDirs
                count++;
                if (callback && callback(count / total)) {
                    return true;
                }
                continue;
            }
            // Comprimo la sotto cartella
            if(!compressSubDir(zip,file.absoluteFilePath(),origDir,recursive,filters, [&](qreal progress)->bool {
                if (callback && callback((progress + count) / total)) {
                    return true;
                }
            })) {
                return false;
            } else {
                count++;
            }
        }
    }

    // Per ogni file nella cartella
    QFileInfoList files = directory.entryInfoList(QDir::Files|filters);
    for (const auto& file : files) {
        // Se non e un file o e il file compresso che sto creando
        if(!file.isFile()||file.absoluteFilePath()==zip->getZipName()) {
            count++;
            if (callback && callback(count / total)) {
                return true;
            }
            continue;
        }

        // Creo il nome relativo da usare all'interno del file compresso
        QString filename = origDirectory.relativeFilePath(file.absoluteFilePath());

        // Comprimo il file
        if (!compressFile(zip,file.absoluteFilePath(),filename, [&](qreal progress)->bool {
            if (callback && callback((progress + count) / total)) {
                return true;
            }
            return false;
        })) {
            return false;
        } else {
            count++;
        }
    }

    return true;
}

bool JlCompress::extractFile(QuaZip* zip, QString fileName, QString fileDest, const QuazipProgressCallback& callback) {
    // zip: oggetto dove aggiungere il file
    // filename: nome del file reale
    // fileincompress: nome del file all'interno del file compresso

    // Controllo l'apertura dello zip
    if (!zip) return false;
    if (zip->getMode()!=QuaZip::mdUnzip) return false;

    // Apro il file compresso
    if (!fileName.isEmpty())
        zip->setCurrentFile(fileName);
    QuaZipFile inFile(zip);
    if(!inFile.open(QIODevice::ReadOnly) || inFile.getZipError()!=UNZ_OK) return false;

    // Controllo esistenza cartella file risultato
    QDir curDir;
    if (fileDest.endsWith(QLatin1String("/"))) {
        if (!curDir.mkpath(fileDest)) {
            return false;
        }
    } else {
        if (!curDir.mkpath(QFileInfo(fileDest).absolutePath())) {
            return false;
        }
    }

    QuaZipFileInfo64 info;
    if (!zip->getCurrentFileInfo(&info))
        return false;

    QFile::Permissions srcPerm = info.getPermissions();
    if (fileDest.endsWith(QLatin1String("/")) && QFileInfo(fileDest).isDir()) {
        if (srcPerm != 0) {
            QFile(fileDest).setPermissions(srcPerm);
        }
        return true;
    }

    if (info.isSymbolicLink()) {
        QString target = QFile::decodeName(inFile.readAll());
        return QFile::link(target, fileDest);
    }

    // Apro il file risultato
    QFile outFile;
    outFile.setFileName(fileDest);
    if(!outFile.open(QIODevice::WriteOnly)) return false;

    // Copio i dati
    if (!copyData(inFile, outFile, callback) || inFile.getZipError()!=UNZ_OK) {
        outFile.close();
        removeFile(QStringList(fileDest));
        return false;
    }
    outFile.close();

    // Chiudo i file
    inFile.close();
    if (inFile.getZipError()!=UNZ_OK) {
        removeFile(QStringList(fileDest));
        return false;
    }

    if (srcPerm != 0) {
        outFile.setPermissions(srcPerm);
    }
    return true;
}

bool JlCompress::removeFile(QStringList listFile) {
    bool ret = true;
    // Per ogni file
    for (int i=0; i<listFile.count(); i++) {
        // Lo elimino
        ret = ret && QFile::remove(listFile.at(i));
    }
    return ret;
}

bool JlCompress::compressFile(QString fileCompressed, QString file, const QuazipProgressCallback& callback) {
    // Creo lo zip
    QuaZip zip(fileCompressed);
    QDir().mkpath(QFileInfo(fileCompressed).absolutePath());
    if(!zip.open(QuaZip::mdCreate)) {
        QFile::remove(fileCompressed);
        return false;
    }

    // Aggiungo il file
    if (!compressFile(&zip,file,QFileInfo(file).fileName()), callback) {
        QFile::remove(fileCompressed);
        return false;
    }

    // Chiudo il file zip
    zip.close();
    if(zip.getZipError()!=0) {
        QFile::remove(fileCompressed);
        return false;
    }

    return true;
}

bool JlCompress::compressFiles(QString fileCompressed, QStringList files, const QuazipProgressCallback& callback) {
    // Creo lo zip
    QuaZip zip(fileCompressed);
    QDir().mkpath(QFileInfo(fileCompressed).absolutePath());
    if(!zip.open(QuaZip::mdCreate)) {
        QFile::remove(fileCompressed);
        return false;
    }

    // Comprimo i file
    QFileInfo info;
    int size = files.size();
    for (int index = 0; index < size; ++index ) {
        const QString & file( files.at( index ) );
        info.setFile(file);
        if (!info.exists() || !compressFile(&zip,file,info.fileName(), [&](qreal progress) -> bool {
            if (callback && callback((index + progress) / size)) {
                return true;
            }
            return false;
        })) {
            QFile::remove(fileCompressed);
            return false;
        }
    }

    // Chiudo il file zip
    zip.close();
    if(zip.getZipError()!=0) {
        QFile::remove(fileCompressed);
        return false;
    }

    return true;
}

bool JlCompress::compressDir(QString fileCompressed, QString dir, bool recursive, const QuazipProgressCallback& callback) {
    return compressDir(fileCompressed, dir, recursive, QDir::Filters(), callback);
}

bool JlCompress::compressDir(QString fileCompressed, QString dir,
                             bool recursive, QDir::Filters filters, const QuazipProgressCallback& callback)
{
    // Creo lo zip
    QuaZip zip(fileCompressed);
    QDir().mkpath(QFileInfo(fileCompressed).absolutePath());
    if(!zip.open(QuaZip::mdCreate)) {
        QFile::remove(fileCompressed);
        return false;
    }

    // Aggiungo i file e le sotto cartelle
    if (!compressSubDir(&zip,dir,dir,recursive, filters, callback)) {
        QFile::remove(fileCompressed);
        return false;
    }

    // Chiudo il file zip
    zip.close();
    if(zip.getZipError()!=0) {
        QFile::remove(fileCompressed);
        return false;
    }

    return true;
}

QString JlCompress::extractFile(QString fileCompressed, QString fileName, QString fileDest, const QuazipProgressCallback& callback) {
    // Apro lo zip
    QuaZip zip(fileCompressed);
    return extractFile(zip, fileName, fileDest, callback);
}

QString JlCompress::extractFile(QuaZip &zip, QString fileName, QString fileDest, const QuazipProgressCallback& callback)
{
    if(!zip.open(QuaZip::mdUnzip)) {
        return QString();
    }

    // Estraggo il file
    if (fileDest.isEmpty())
        fileDest = fileName;
    if (!extractFile(&zip,fileName,fileDest,callback)) {
        return QString();
    }

    // Chiudo il file zip
    zip.close();
    if(zip.getZipError()!=0) {
        removeFile(QStringList(fileDest));
        return QString();
    }
    return QFileInfo(fileDest).absoluteFilePath();
}

QStringList JlCompress::extractFiles(QString fileCompressed, QStringList files, QString dir, const QuazipProgressCallback& callback) {
    // Creo lo zip
    QuaZip zip(fileCompressed);
    return extractFiles(zip, files, dir, callback);
}

QStringList JlCompress::extractFiles(QuaZip &zip, const QStringList &files, const QString &dir, const QuazipProgressCallback& callback)
{
    if(!zip.open(QuaZip::mdUnzip)) {
        return QStringList();
    }

    // Estraggo i file
    QStringList extracted;
    int size = files.count();
    for (int i=0; i<files.count(); i++) {
        QString absPath = QDir(dir).absoluteFilePath(files.at(i));
        if (!extractFile(&zip, files.at(i), absPath, [&](qreal progress)->bool{
            if (callback && callback((i + progress) / size)) {
                return true;
            }
            return false;
        })) {
            removeFile(extracted);
            return QStringList();
        }
        extracted.append(absPath);
    }

    // Chiudo il file zip
    zip.close();
    if(zip.getZipError()!=0) {
        removeFile(extracted);
        return QStringList();
    }

    return extracted;
}

QStringList JlCompress::extractDir(QString fileCompressed, QTextCodec* fileNameCodec, QString dir, const QuazipProgressCallback& callback) {
    // Apro lo zip
    QuaZip zip(fileCompressed);
    if (fileNameCodec)
        zip.setFileNameCodec(fileNameCodec);
    return extractDir(zip, dir, callback);
}

QStringList JlCompress::extractDir(QString fileCompressed, QString dir, const QuazipProgressCallback& callback) {
    return extractDir(fileCompressed, nullptr, dir, callback);
}

QStringList JlCompress::extractDir(QuaZip &zip, const QString &dir, const QuazipProgressCallback& callback)
{
    if(!zip.open(QuaZip::mdUnzip)) {
        return QStringList();
    }
    QString cleanDir = QDir::cleanPath(dir);
    QDir directory(cleanDir);
    QString absCleanDir = directory.absolutePath();
    if (!absCleanDir.endsWith(QLatin1Char('/'))) // It only ends with / if it's the FS root.
        absCleanDir += QLatin1Char('/');
    QStringList extracted;
    int count = 0;
    int size = zip.getFileNameList().size();
    if (!zip.goToFirstFile()) {
        return QStringList();
    }
    do {
        QString name = zip.getCurrentFileName();
        QString absFilePath = directory.absoluteFilePath(name);
        QString absCleanPath = QDir::cleanPath(absFilePath);
        if (!absCleanPath.startsWith(absCleanDir)) {
            if (callback) {
                callback((count + 1.0) / size);
            }
            continue;
        }

        if (!extractFile(&zip, QLatin1String(""), absFilePath, [&](qreal progress){
            if (callback && callback((count + progress) / size)) {
                return true;
            }
            return false;
        })) {
            removeFile(extracted);
            return QStringList();
        }
        extracted.append(absFilePath);
    } while (zip.goToNextFile() && ++count);

    // Chiudo il file zip
    zip.close();
    if(zip.getZipError()!=0) {
        removeFile(extracted);
        return QStringList();
    }

    return extracted;
}

QStringList JlCompress::getFileList(QString fileCompressed) {
    // Apro lo zip
    QuaZip* zip = new QuaZip(QFileInfo(fileCompressed).absoluteFilePath());
    return getFileList(zip);
}

QStringList JlCompress::getFileList(QuaZip *zip)
{
    if(!zip->open(QuaZip::mdUnzip)) {
        delete zip;
        return QStringList();
    }

    // Estraggo i nomi dei file
    QStringList lst;
    QuaZipFileInfo64 info;
    for(bool more=zip->goToFirstFile(); more; more=zip->goToNextFile()) {
      if(!zip->getCurrentFileInfo(&info)) {
          delete zip;
          return QStringList();
      }
      lst << info.name;
      //info.name.toLocal8Bit().constData()
    }

    // Chiudo il file zip
    zip->close();
    if(zip->getZipError()!=0) {
        delete zip;
        return QStringList();
    }
    delete zip;
    return lst;
}

QStringList JlCompress::extractDir(QIODevice* ioDevice, QTextCodec* fileNameCodec, QString dir, const QuazipProgressCallback& callback)
{
    QuaZip zip(ioDevice);
    if (fileNameCodec)
        zip.setFileNameCodec(fileNameCodec);
    return extractDir(zip, dir, callback);
}

QStringList JlCompress::extractDir(QIODevice *ioDevice, QString dir, const QuazipProgressCallback& callback)
{
    return extractDir(ioDevice, nullptr, dir, callback);
}

QStringList JlCompress::getFileList(QIODevice *ioDevice)
{
    QuaZip *zip = new QuaZip(ioDevice);
    return getFileList(zip);
}

QString JlCompress::extractFile(QIODevice *ioDevice, QString fileName, QString fileDest, const QuazipProgressCallback& callback)
{
    QuaZip zip(ioDevice);
    return extractFile(zip, fileName, fileDest, callback);
}

QStringList JlCompress::extractFiles(QIODevice *ioDevice, QStringList files, QString dir, const QuazipProgressCallback& callback)
{
    QuaZip zip(ioDevice);
    return extractFiles(zip, files, dir, callback);
} 
