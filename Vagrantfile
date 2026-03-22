BOXES = {
  "freebsd14" => { box: "generic/freebsd14", shell: "sh",  synced: false },
  "netbsd9"   => { box: "generic/netbsd9",   shell: "sh",  synced: false },
  "alpine319" => { box: "generic/alpine319", shell: "sh",  synced: true  },
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
  "alpine319" => <<~SH,
    sudo apk add --no-cache make gcc musl-dev ncurses-dev git
    cd /vagrant && make
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
