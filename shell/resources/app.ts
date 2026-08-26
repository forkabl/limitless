const shell = document.querySelector('#limitless-shell');

if (!(shell instanceof HTMLElement)) {
  throw new Error('Limitless shell root is missing.');
}

shell.dataset['ready'] = 'true';
