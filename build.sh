make

/usr/local/bin/install4jc *.install4j

cd distribution

rm -rf tmp
mkdir tmp
dpkg-deb -R *.deb tmp
rm -rf tmp/.install4j
dpkg-deb -b tmp *.deb
