BOXES = {
  "freebsd14" => { box: "generic/freebsd14", shell: "sh",  synced: false },
  "netbsd9"   => { box: "generic/netbsd9",   shell: "sh",  synced: false },
  "openbsd7"  => { box: "generic/openbsd7",  shell: "sh",  synced: false },
}

PROVISION = {
  "freebsd14" => <<~SH,
    sudo pkg upgrade -y
    sudo pkg install -y gmake ncurses git pkgconf
    git clone https://github.com/hboetes/mg /home/vagrant/mg
    cd /home/vagrant/mg && gmake
  SH
  "netbsd9" => <<~SH,
    echo "https://ftp.NetBSD.org/pub/pkgsrc/packages/NetBSD/x86_64/9.3/All" | sudo tee /usr/pkg/etc/pkgin/repositories.conf
    sudo pkgin -y install git mozilla-rootcerts-openssl pkgconf ncurses libbsd gmake
    sudo update-ca-certificates
    git clone https://github.com/hboetes/mg /home/vagrant/mg
    cd /home/vagrant/mg && gmake
  SH
  "openbsd7" => <<~SH,
    command -v cvs >/dev/null 2>&1 || doas pkg_add cvs
    mkdir -p ~/.ssh
    ssh-keyscan -H anoncvs.eu.openbsd.org >> ~/.ssh/known_hosts 2>/dev/null
    export CVS_RSH="ssh"
    mkdir -p /home/vagrant/obsd-src
    cd /home/vagrant/obsd-src
    cvs -qd anoncvs@anoncvs.eu.openbsd.org:/cvs get -P src/usr.bin/mg
    cd src/usr.bin/mg
    # patch -p0 < /home/vagrant/mg-require-final-newline-upstream.patch
    make
  SH
}

Vagrant.configure("2") do |config|
  BOXES.each do |name, cfg|
    config.vm.define name do |box|
      box.vm.box = cfg[:box]
      box.ssh.shell = cfg[:shell]
      box.vm.synced_folder ".", "/vagrant", disabled: !cfg[:synced]

      box.vm.provider :libvirt do |lv|
        lv.memory = 512
        lv.cpus = 2
      end

      box.vm.provision "shell", privileged: false, inline: PROVISION[name]
    end
  end
end
