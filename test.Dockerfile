FROM ubuntu:22.04
LABEL maintainer="Vlad Lunculescu"
WORKDIR /app

ENV NODE_ENV=production

COPY src /app

RUN apt-get update && apt-get install -y \
    curl \
    nginx \
    && apt-get clean \
    && rm -rf /var/lib/apt/lists/*

CMD ["/bin/bash"]

