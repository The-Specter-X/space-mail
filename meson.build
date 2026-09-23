project('space-mail', 'c', version: '0.1.0',
  default_options: ['c_std=c11', 'warning_level=3'])

gtk = dependency('gtk+-3.0')
xapp = dependency('xapp')
gmime = dependency('gmime-3.0')
secret = dependency('libsecret-1')
sqlite = dependency('sqlite3')
curl = dependency('libcurl')
xml = dependency('libxml-2.0')

executable('space-mail',
  ['src/main.c', 'src/store.c', 'src/network.c', 'src/imap.c'],
  dependencies: [gtk, xapp, gmime, secret, sqlite, curl, xml],
  install: true)

store_test = executable('store-test',
  ['tests/store-test.c', 'src/store.c', 'src/network.c', 'src/imap.c'],
  dependencies: [gmime, secret, sqlite, curl, xml, gtk])
test('offline mail and outbox', store_test)

install_data('data/org.axionis.SpaceMail.desktop',
  install_dir: get_option('datadir') / 'applications')
